#pragma once

#include <liburing.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <span>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

namespace loomstore {

// Configuration constants
constexpr size_t RING_SIZE = 4096;
constexpr size_t MAX_CONNECTIONS = 65536;
constexpr size_t BUFFER_SIZE = 8192;  // 8KB per buffer
constexpr size_t BUFFER_POOL_SIZE = 1024;
constexpr int SQ_THREAD_IDLE = 2000;  // ms

// Forward declarations
class Connection;
class Request;
class Response;

// Core I/O uring engine with zero-copy networking
class IOUrgingEngine {
public:
    IOUrgingEngine(uint16_t port, size_t workers = std::thread::hardware_concurrency());
    ~IOUrgingEngine();

    // Disable copy/move
    IOUrgingEngine(const IOUrgingEngine&) = delete;
    IOUrgingEngine& operator=(const IOUrgingEngine&) = delete;

    // Start the event loop
    void start();

    // Graceful shutdown
    void stop();

    // Statistics
    struct Stats {
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> active_connections{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> avg_latency_us{0};
    };

    const Stats& get_stats() const { return stats_; }

private:
    // I/O uring instances (one per thread for better scalability)
    struct PerThreadData {
        struct io_uring ring;
        int eventfd;
        std::vector<std::unique_ptr<char[]>> buffer_pool;
        std::atomic<bool> running{true};
    };

    uint16_t port_;
    size_t workers_;
    std::vector<std::thread> worker_threads_;
    std::vector<std::unique_ptr<PerThreadData>> thread_data_;
    int listen_fd_;
    Stats stats_;

    // Initialize socket with optimal settings
    bool setup_listener();
    
    // Worker thread main loop
    void worker_thread(size_t thread_id);
    
    // Submit accept request
    void submit_accept(struct io_uring* ring, int listen_fd);
    
    // Handle completed operations
    void handle_completion(struct io_uring_cqe* cqe, PerThreadData* data);
    
    // Process application-level request
    void process_request(const uint8_t* data, size_t len, int client_fd, PerThreadData* thread_data);
};

} // namespace loomstore
