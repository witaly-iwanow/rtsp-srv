#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

// Thread-safe set of server-side RTP/RTCP ports currently reserved by active sessions.
// Closes the cross-session TOCTOU window between allocate_ports and the actual streamer bind.
class PortRegistry {
public:
    bool try_reserve(std::uint16_t port) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ports_.insert(port).second;
    }

    void release(std::uint16_t port) {
        std::lock_guard<std::mutex> lock(mutex_);
        ports_.erase(port);
    }

private:
    std::mutex mutex_;
    std::unordered_set<std::uint16_t> ports_;
};
