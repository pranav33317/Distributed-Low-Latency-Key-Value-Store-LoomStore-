#include "core/lockfree_map.hpp"
#include <cstring>

namespace loomstore {

template<typename K, typename V, typename Hash>
LockFreeMap<K, V, Hash>::LockFreeMap(size_t capacity) 
    : capacity_(capacity) {
    // Ensure capacity is power of two
    capacity_ = 1;
    while (capacity_ < capacity) {
        capacity_ <<= 1;
    }
    
    // Allocate buckets with huge pages if possible
    size_t total_size = capacity_ * sizeof(Bucket);
    void* ptr = nullptr;
    
    // Try to use huge pages for better TLB performance
    #ifdef MADV_HUGEPAGE
    ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr != MAP_FAILED) {
        madvise(ptr, total_size, MADV_HUGEPAGE);
        buckets_.reset(reinterpret_cast<Bucket*>(ptr));
    } else
    #endif
    {
        buckets_.reset(new Bucket[capacity_]());
    }
    
    // Initialize buckets
    for (size_t i = 0; i < capacity_; ++i) {
        for (int j = 0; j < BUCKET_SIZE; ++j) {
            buckets_[i].slots[j].version.store(0, std::memory_order_relaxed);
        }
    }
}

template<typename K, typename V, typename Hash>
LockFreeMap<K, V, Hash>::~LockFreeMap() = default;

template<typename K, typename V, typename Hash>
bool LockFreeMap<K, V, Hash>::insert(const K& key, const V& value) {
    size_t bucket_idx = hash_to_bucket(key);
    Bucket& bucket = buckets_[bucket_idx];
    
    // Try to find empty slot using CAS
    for (int attempt = 0; attempt < 3; ++attempt) {
        // First check if key already exists
        uint64_t version;
        int slot_idx = bucket.find_slot(key, version);
        
        if (slot_idx >= 0) {
            // Update existing slot
            KVSlot<K, V>& slot = bucket.slots[slot_idx];
            
            // Use double-width CAS if available (128-bit)
            struct alignas(16) KV {
                K key;
                V value;
            } old_kv, new_kv;
            
            old_kv.key = slot.key;
            old_kv.value = slot.value;
            new_kv.key = key;
            new_kv.value = value;
            
            // Atomic 128-bit compare-exchange
            #ifdef __x86_64__
            bool success = __atomic_compare_exchange_16(
                reinterpret_cast<__int128*>(&slot),
                reinterpret_cast<__int128*>(&old_kv),
                reinterpret_cast<__int128*>(&new_kv),
                false,
                __ATOMIC_SEQ_CST,
                __ATOMIC_SEQ_CST
            );
            #else
            // Fallback for non-x86
            slot.key = key;
            slot.value = value;
            bool success = true;
            #endif
            
            if (success) {
                slot.version.fetch_add(1, std::memory_order_release);
                return true;
            }
            continue;
        }
        
        // Find empty slot
        for (int i = 0; i < BUCKET_SIZE; ++i) {
            KVSlot<K, V>& slot = bucket.slots[i];
            bool expected = false;
            
            if (slot.occupied.compare_exchange_strong(expected, true,
                                                       std::memory_order_acq_rel)) {
                // Successfully claimed empty slot
                slot.key = key;
                slot.value = value;
                slot.version.fetch_add(1, std::memory_order_release);
                size_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        
        // Bucket full - linear probe to next bucket
        bucket_idx = (bucket_idx + 1) & (capacity_ - 1);
        bucket = buckets_[bucket_idx];
    }
    
    return false;  // Table full
}

template<typename K, typename V, typename Hash>
bool LockFreeMap<K, V, Hash>::find(const K& key, V& out_value) const {
    size_t bucket_idx = hash_to_bucket(key);
    const Bucket* bucket = &buckets_[bucket_idx];
    
    // Linear probing
    for (int probe = 0; probe < 8; ++probe) {
        uint64_t version;
        int slot_idx = bucket->find_slot(key, version);
        
        if (slot_idx >= 0) {
            const KVSlot<K, V>& slot = bucket->slots[slot_idx];
            
            // Read with version check for consistency
            uint64_t v1 = slot.version.load(std::memory_order_acquire);
            if (v1 != version) continue;  // Version changed
            
            out_value = slot.value;
            
            uint64_t v2 = slot.version.load(std::memory_order_acquire);
            if (v1 == v2 && v1 % 2 == 0) {  // Not being updated
                return true;
            }
        }
        
        // Move to next bucket
        bucket_idx = (bucket_idx + 1) & (capacity_ - 1);
        bucket = &buckets_[bucket_idx];
    }
    
    return false;
}

template<typename K, typename V, typename Hash>
bool LockFreeMap<K, V, Hash>::erase(const K& key) {
    size_t bucket_idx = hash_to_bucket(key);
    Bucket* bucket = &buckets_[bucket_idx];
    
    for (int probe = 0; probe < 8; ++probe) {
        uint64_t version;
        int slot_idx = bucket->find_slot(key, version);
        
        if (slot_idx >= 0) {
            KVSlot<K, V>& slot = bucket->slots[slot_idx];
            
            // Mark as unoccupied
            slot.occupied.store(false, std::memory_order_release);
            slot.version.fetch_add(1, std::memory_order_release);
            size_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        
