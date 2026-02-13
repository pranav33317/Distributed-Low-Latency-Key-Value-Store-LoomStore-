#include "core/io_uring_engine.hpp"
#include "core/lockfree_map.hpp"
#include "core/slab_allocator.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cstring>
#include <iostream>

namespace loomstore {

IOUrgingEngine::IOUrgingEngine(uint16_t port, size_t workers) 
    : port_(port), workers_(workers) {
    
    // Setup listening socket
    if (!setup_listener()) {
        throw std::runtime_error("Failed to setup listener");
    }
    
    // Initialize per-thread data
    for (size_t i = 0; i < workers_; ++i) {
        auto data = std::make_unique<PerThreadData>();
        
        // Initialize io_uring with submission queue polling
        struct io_uring_params params = {};
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = SQ_THREAD_IDLE;
        
        if (io_uring_queue_init_params(RING_SIZE, &data->ring, &params) < 0) {
            throw std::runtime_error("Failed to init io_uring");
        }
        
        // Create eventfd for shutdown notification
        data->eventfd = eventfd(0, EFD_NONBLOCK);
        
        // Allocate buffer pool
        for (size_t j = 0; j < BUFFER_POOL_SIZE; ++j) {
            data->buffer_pool.push_back(std::make_unique<char[]>(BUFFER_SIZE));
            
            // Register buffer for zero-copy
            struct io_uring_sqe* sqe = io_uring_get_sqe(&data->ring);
            io_uring_prep_provide_buffers(sqe, data->buffer_pool.back().get(), 
                                          BUFFER_SIZE, 1, j, 0);
        }
        
        thread_data_.push_back(std::move(data));
    }
}

IOUrgingEngine::~IOUrgingEngine() {
    stop();
    if (listen_fd_ >= 0) close(listen_fd_);
}

bool IOUrgingEngine::setup_listener() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) return false;
    
    // Set socket options for maximum performance
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    // Disable Nagle's algorithm
    opt = 1;
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    // Increase buffer sizes
    int rcvbuf = 4 * 1024 * 1024;  // 4MB
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(listen_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(listen_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listen_fd_);
        return false;
    }
    
    if (listen(listen_fd_, 1024) < 0) {
        close(listen_fd_);
        return false;
    }
    
    return true;
}

void IOUrgingEngine::submit_accept(struct io_uring* ring, int listen_fd) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    io_uring_prep_accept(sqe, listen_fd, (struct sockaddr*)&client_addr, &client_len, 0);
    io_uring_sqe_set_data(sqe, (void*)1);  // Mark as accept request
}

void IOUrgingEngine::worker_thread(size_t thread_id) {
    auto* data = thread_data_[thread_id].get();
    
    // Set CPU affinity for this thread
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(thread_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    // Register eventfd for shutdown
    struct io_uring_sqe* sqe = io_uring_get_sqe(&data->ring);
    io_uring_prep_read(sqe, data->eventfd, &data->eventfd, sizeof(uint64_t), 0);
    
    // Submit initial accept
    submit_accept(&data->ring, listen_fd_);
    io_uring_submit(&data->ring);
    
    while (data->running.load(std::memory_order_relaxed)) {
        struct io_uring_cqe* cqes[32];
        int ret = io_uring_wait_cqe(&data->ring, &cqes[0]);
        if (ret < 0) continue;
        
        // Get multiple completions at once
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&data->ring, head, cqes[count]) {
            count++;
        }
        
        // Process completions
        for (unsigned i = 0; i < count; ++i) {
            handle_completion(cqes[i], data);
        }
        
        // Mark all as seen
        io_uring_cq_advance(&data->ring, count);
    }
}

void IOUrgingEngine::handle_completion(struct io_uring_cqe* cqe, PerThreadData* data) {
    void* user_data = io_uring_cqe_get_data(cqe);
    int ret = cqe->res;
    
    if (ret < 0) {
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    
    if (user_data == (void*)1) {  // Accept completion
        if (ret > 0) {
            int client_fd = ret;
            stats_.active_connections.fetch_add(1, std::memory_order_relaxed);
            
            // Submit read request for new connection
            struct io_uring_sqe* sqe = io_uring_get_sqe(&data->ring);
            int buffer_id = (stats_.total_requests.load() % BUFFER_POOL_SIZE);
            io_uring_prep_recv(sqe, client_fd, data->buffer_pool[buffer_id].get(), 
                              BUFFER_SIZE, 0);
            io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
            sqe->buf_group = 0;
            io_uring_sqe_set_data(sqe, (void*)(uintptr_t)client_fd);
            
            // Submit next accept
            submit_accept(&data->ring, listen_fd_);
        }
    } else {  // Read completion
        int client_fd = (int)(uintptr_t)user_data;
        uint8_t* buffer = (uint8_t*)io_uring_cqe_get_buf(cqe, data->ring);
        
        if (ret > 0) {
            // Process the request
            auto start = std::chrono::steady_clock::now();
            process_request(buffer, ret, client_fd, data);
            auto end = std::chrono::steady_clock::now();
            
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
            // Update stats (moving average)
            stats_.avg_latency_us.store(
                (stats_.avg_latency_us.load() * 7 + latency) / 8,
                std::memory_order_relaxed
            );
            
            stats_.total_requests.fetch_add(1, std::memory_order_relaxed);
            stats_.bytes_received.fetch_add(ret, std::memory_order_relaxed);
        }
        
        // Close connection on error or EOF
        if (ret <= 0) {
            close(client_fd);
            stats_.active_connections.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void IOUrgingEngine::process_request(const uint8_t* data, size_t len, int client_fd, PerThreadData* thread_data) {
    // Parse request (simplified - assume first byte is opcode)
    uint8_t opcode = data[0];
    
    // Allocate response buffer from slab
    auto& alloc = SlabAllocator::instance();
    uint8_t* response = static_cast<uint8_t*>(alloc.allocate(256));
    
    size_t response_len = 0;
    
    switch (opcode) {
        case 0x01: {  // GET
            // Process GET request...
            response[0] = 0x81;  // Response opcode
            response_len = 1;
            break;
        }
        case 0x02: {  // PUT
            // Process PUT request...
            response[0] = 0x82;
            response_len = 1;
            break;
        }
        default:
            response[0] = 0xFF;  // Error
            response_len = 1;
    }
    
    // Submit write
    struct io_uring_sqe* sqe = io_uring_get_sqe(&thread_data->ring);
    io_uring_prep_send(sqe, client_fd, response, response_len, 0);
    io_uring_sqe_set_data(sqe, (void*)(uintptr_t)client_fd);
    
    // Update stats
    stats_.bytes_sent.fetch_add(response_len, std::memory_order_relaxed);
    
    // Deallocate response buffer
    alloc.deallocate(response, 256);
}

void IOUrgingEngine::start() {
    // Start worker threads
    for (size_t i = 0; i < workers_; ++i) {
        worker_threads_.emplace_back(&IOUrgingEngine::worker_thread, this, i);
    }
}

void IOUrgingEngine::stop() {
    // Signal all threads to stop
    for (auto& data : thread_data_) {
        data->running.store(false, std::memory_order_relaxed);
        uint64_t val = 1;
        write(data->eventfd, &val, sizeof(val));
    }
    
    // Join threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // Cleanup io_uring instances
    for (auto& data : thread_data_) {
        io_uring_queue_exit(&data->ring);
        close(data->eventfd);
    }
}

} // namespace loomstore
