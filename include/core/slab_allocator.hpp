#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace loomstore {

// Size classes for slab allocation
enum class SizeClass : uint8_t {
    SIZE_64 = 0,
    SIZE_128,
    SIZE_256,
    SIZE_512,
    SIZE_1024,
    SIZE_2048,
    SIZE_4096,
    COUNT
};

constexpr std::array<size_t, static_cast<size_t>(SizeClass::COUNT)> SIZE_CLASSES = {
    64, 128, 256, 512, 1024, 2048, 4096
};

// Per-CPU slab cache
class alignas(64) SlabCache {
public:
    SlabCache();
    ~SlabCache();

    // Allocate memory of given size
    void* allocate(size_t size);
    
    // Deallocate memory
    void deallocate(void* ptr, size_t size);
    
    // Statistics
    size_t get_allocation_count() const { return allocations_.load(std::memory_order_relaxed); }

private:
    // Slab page (typically 4MB)
    struct SlabPage {
        std::atomic<SlabPage*> next{nullptr};
        size_t size_class;
        uint8_t* free_list;
        size_t free_count;
        uint8_t data[];  // Flexible array member
    };

    // Per-size class freelist
    struct SizeClassInfo {
        std::atomic<SlabPage*> partial_pages{nullptr};
        std::atomic<SlabPage*> full_pages{nullptr};
        std::atomic<SlabPage*> empty_pages{nullptr};
    };

    SizeClassInfo classes_[static_cast<size_t>(SizeClass::COUNT)];
    std::atomic<size_t> allocations_{0};
    
    // Get size class for allocation
    SizeClass get_size_class(size_t size);
    
    // Allocate new slab page
    SlabPage* allocate_new_page(SizeClass sc);
    
    // Thread-local cache for fast allocation
    static thread_local struct {
        void* buffer[64];  // Small thread-local cache
        size_t count;
    } tls_cache_;
};

// Global slab allocator (singleton)
class SlabAllocator {
public:
    static SlabAllocator& instance() {
        static SlabAllocator inst;
        return inst;
    }

    void* allocate(size_t size) {
        return get_cache().allocate(size);
    }

    void deallocate(void* ptr, size_t size) {
        get_cache().deallocate(ptr, size);
    }

private:
    SlabAllocator() = default;
    
    // Get or create per-CPU cache
    SlabCache& get_cache() {
        thread_local std::unique_ptr<SlabCache> cache(new SlabCache());
        return *cache;
    }
};

} // namespace loomstore
