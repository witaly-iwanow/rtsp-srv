#pragma once

#include <filesystem>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>
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
    struct TrackPorts {
        std::uint16_t client_rtp = 0;
        std::uint16_t client_rtcp = 0;
        std::uint16_t server_rtp = 0;
        std::uint16_t server_rtcp = 0;
        bool setup = false;
    };

    bool send_raw(const std::string& data);
    bool send_response(
        int status_code,
        const std::string& reason,
        const std::string& cseq,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body);
    bool start_streaming();
    void stop_streaming();
    bool handle_request(const std::string& raw_request, bool& should_close);
    static std::string make_session_id();

    int client_fd_;
    std::string remote_endpoint_;
    std::filesystem::path media_dir_;
    std::string session_id_;
    std::string current_media_uri_;
    TrackPorts video_ports_ {0, 0, 50000, 50001, false};
    TrackPorts audio_ports_ {0, 0, 50002, 50003, false};
    std::filesystem::path current_media_path_;
    pid_t ffmpeg_pid_ = -1;
    mutable std::mutex stream_mutex_;
    mutable std::mutex fd_mutex_;
};
