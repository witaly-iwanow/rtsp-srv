#pragma once

#include <mutex>
#include <string>

class Session {
public:
    Session(int client_fd, const std::string& remote_endpoint):
        client_fd_(client_fd), remote_endpoint_(remote_endpoint) {}

    ~Session();

    void run();
    void shutdown();
    const std::string& remote_endpoint() const;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:
    int client_fd_;
    std::string remote_endpoint_;
    mutable std::mutex fd_mutex_;
};
