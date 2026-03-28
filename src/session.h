#pragma once

#include <filesystem>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class Session {
public:
    Session(int client_fd, const std::string& remote_endpoint, const std::filesystem::path& media_dir):
        client_fd_(client_fd), remote_endpoint_(remote_endpoint), media_dir_(media_dir) {}

    ~Session();

    void run();
    void shutdown();
    const std::string& remote_endpoint() const;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:
    bool send_raw(const std::string& data);
    bool send_response(
        int status_code,
        const std::string& reason,
        const std::string& cseq,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body);
    bool handle_request(const std::string& raw_request, bool& should_close);
    static std::string make_session_id();

    int client_fd_;
    std::string remote_endpoint_;
    std::filesystem::path media_dir_;
    std::string session_id_;
    std::string current_media_uri_;
    std::uint16_t client_rtp_port_ = 0;
    std::uint16_t client_rtcp_port_ = 0;
    std::uint16_t server_rtp_port_ = 50000;
    std::uint16_t server_rtcp_port_ = 50001;
    mutable std::mutex fd_mutex_;
};
