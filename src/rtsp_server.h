#pragma once

#include "session.h"

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RtspServer {
public:
    RtspServer(const std::filesystem::path& media_dir, const std::string& host, std::uint16_t port):
        media_dir_(media_dir), host_(host), port_(port) {};

    int run();

private:
    struct SessionWorker {
        std::unique_ptr<Session> session;
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void start_session(int client_fd, const std::string& remote_endpoint);
    void cleanup_stale_sessions();
    void shutdown_sessions();

    std::filesystem::path media_dir_;
    std::string host_;
    std::uint16_t port_;
    std::mutex sessions_mutex_;
    std::condition_variable sessions_cv_;
    std::vector<SessionWorker> sessions_;
    std::atomic<int> active_sessions_ {0};
    std::atomic<bool> run_called_ {false};
    bool stop_reaper_ = false;
};
