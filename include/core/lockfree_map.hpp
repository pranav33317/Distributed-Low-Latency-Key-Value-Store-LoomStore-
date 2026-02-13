#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <immintrin.h>

namespace loomstore {

// Cache line size for alignment
constexpr size_t CACHE_LINE_SIZE = 64;

// Bucket size for SIMD operations
constexpr size_t BUCKET_SIZE = 16;  // 16 keys per bucket for AVX2

// Key-value pair with cache alignment
template<typename K, typename V>
struct alignas(CACHE_LINE_SIZE) KVSlot {
    std::atomic<uint64_t> version{0};  // For ABA prevention
    K key;
    V value;
    std::atomic<bool> occupied{false};
    uint8_t padding[CACHE_LINE_SIZE - sizeof(K) - sizeof(V) - sizeof(std::atomic<bool>) - sizeof(std::atomic<uint64_t>)];
};

// Lock-free concurrent hash map with SIMD acceleration
template<typename K, typename V, typename Hash = std::hash<K>>
class LockFreeMap {
    static_assert(sizeof(K) <= 32, "Key size must fit in AVX2 register");
    static_assert(std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>);

public:
    explicit LockFreeMap(size_t capacity = 1 << 20);  // 1M slots default
    ~LockFreeMap();

    // Insert or update
    bool insert(const K& key, const V& value);
    
    // Find value
    bool find(const K& key, V& out_value) const;
    
    // Delete key
    bool erase(const K& key);
    
    // Statistics
    size_t size() const { return size_.load(std::memory_order_relaxed); }
    size_t capacity() const { return capacity_; }
    float load_factor() const { return static_cast<float>(size()) / capacity_; }

private:
    struct Bucket {
        alignas(CACHE_LINE_SIZE) KVSlot<K, V> slots[BUCKET_SIZE];
        
        // SIMD-accelerated key search
        int find_slot(const K& key, uint64_t& version) const {
            // Load all keys into AVX2 registers (if key size <= 32 bytes)
            if constexpr (sizeof(K) <= 32) {
                __m256i key_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&key));
                
                // Compare with each slot using SIMD
                for (int i = 0; i < BUCKET_SIZE; i += 4) {
                    __m256i slot_vec = _mm256_loadu_si256(
                        reinterpret_cast<const __m256i*>(&slots[i].key));
                    
                    __m256i cmp = _mm256_cmpeq_epi64(key_vec, slot_vec);
                    int mask = _mm256_movemask_epi8(cmp);
                    
                    if (mask) {
                        // Find exact matching slot
                        for (int j = 0; j < 4; ++j) {
                            if (mask & (0xFF << (j * 8))) {
                                if (slots[i + j].occupied.load(std::memory_order_acquire)) {
                                    version = slots[i + j].version.load(std::memory_order_acquire);
                                    return i + j;
                                }
                            }
                        }
                    }
                }
            } else {
                // Fallback for larger keys
                for (int i = 0; i < BUCKET_SIZE; ++i) {
                    if (slots[i].occupied.load(std::memory_order_acquire)) {
                        if (slots[i].key == key) {
                            version = slots[i].version.load(std::memory_order_acquire);
                            return i;
                        }
                    }
                }
            }
            return -1;
        }
    };

    std::unique_ptr<Bucket[]> buckets_;
    size_t capacity_;
    mutable std::atomic<size_t> size_{0};
    Hash hasher_;

    size_t hash_to_bucket(const K& key) const {
        return hasher_(key) & (capacity_ - 1);
    }
};

} // namespace loomstore
