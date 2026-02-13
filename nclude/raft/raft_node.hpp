#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include <shared_mutex>

namespace loomstore {

// Raft state
enum class RaftState {
    FOLLOWER,
    CANDIDATE,
    LEADER
};

// Log entry
struct LogEntry {
    uint64_t index;
    uint64_t term;
    enum class Type : uint8_t {
        DATA,
        CONFIGURATION,
        NOOP
    } type;
    std::vector<uint8_t> data;
    
    LogEntry() = default;
    LogEntry(uint64_t idx, uint64_t t, Type ty, const uint8_t* d, size_t len)
        : index(idx), term(t), type(ty), data(d, d + len) {}
};

// Raft node configuration
struct RaftConfig {
    uint64_t node_id;
    std::vector<std::pair<uint64_t, std::string>> peers;  // id -> address
    std::chrono::milliseconds election_timeout_min{150};
    std::chrono::milliseconds election_timeout_max{300};
    std::chrono::milliseconds heartbeat_interval{50};
    size_t max_batch_size{128};
    size_t snapshot_threshold{10000};
};

// Raft consensus implementation
class RaftNode {
public:
    explicit RaftNode(const RaftConfig& config);
    ~RaftNode();

    // Start the node
    void start();
    
    // Stop the node
    void stop();

    // Propose a command to the cluster
    bool propose(const uint8_t* data, size_t len, std::chrono::milliseconds timeout);
    
    // Get current leader
    uint64_t get_leader() const { return leader_id_.load(); }
    
    // Get node state
    RaftState get_state() const { return state_.load(); }

private:
    // Persistent state
    std::atomic<uint64_t> current_term_{0};
    std::atomic<uint64_t> voted_for_{0};
    std::vector<LogEntry> log_;
    mutable std::shared_mutex log_mutex_;

    // Volatile state
    std::atomic<uint64_t> commit_index_{0};
    std::atomic<uint64_t> last_applied_{0};
    std::atomic<RaftState> state_{RaftState::FOLLOWER};
    std::atomic<uint64_t> leader_id_{0};

    // Leader volatile state
    std::vector<uint64_t> next_index_;
    std::vector<uint64_t> match_index_;

    // Configuration
    RaftConfig config_;
    std::mt19937_64 rng_;
    std::chrono::milliseconds current_election_timeout_;

    // Threading
    std::thread election_thread_;
    std::thread apply_thread_;
    std::atomic<bool> running_{false};

    // Heartbeat timer for leaders
    std::unique_ptr<std::thread> heartbeat_thread_;
    std::condition_variable_any heartbeat_cv_;
    std::mutex heartbeat_mutex_;

    // RequestVote RPC
    struct RequestVoteRequest {
        uint64_t term;
        uint64_t candidate_id;
        uint64_t last_log_index;
        uint64_t last_log_term;
    };

    struct RequestVoteResponse {
        uint64_t term;
        bool vote_granted;
    };

    // AppendEntries RPC
    struct AppendEntriesRequest {
        uint64_t term;
        uint64_t leader_id;
        uint64_t prev_log_index;
        uint64_t prev_log_term;
        std::vector<LogEntry> entries;
        uint64_t leader_commit;
    };

    struct AppendEntriesResponse {
        uint64_t term;
        bool success;
        uint64_t match_index;
    };

    // Core Raft logic
    void run_election_timer();
    void become_candidate();
    void become_leader();
    void become_follower(uint64_t term);
    void send_request_vote();
    void send_heartbeats();
    void replicate_log();

    // Log operations
    uint64_t get_last_log_index() const;
    uint64_t get_last_log_term() const;
    bool has_newer_log(uint64_t last_index, uint64_t last_term) const;
    
    // RPC handlers
    RequestVoteResponse handle_request_vote(const RequestVoteRequest& req);
    AppendEntriesResponse handle_append_entries(const AppendEntriesRequest& req);
    
    // Apply committed entries to state machine
    void apply_committed_entries();
};

} // namespace loomstore
