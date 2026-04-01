#pragma once

#include <asio.hpp>

#include "logger.h"
#include "media_tools.h"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Session : public std::enable_shared_from_this<Session> {
public:
    using Ptr = std::shared_ptr<Session>;
    using Socket = asio::ip::tcp::socket;
    using CloseHandler = std::function<void(std::uint32_t, const std::string&)>;

    Session(
        Socket socket,
        std::string remote_endpoint,
        const std::filesystem::path& media_dir,
        std::uint32_t session_id,
        CloseHandler on_close);

    ~Session();

    void start();
    void shutdown();
    const std::string& remote_endpoint() const;
    std::uint32_t session_id() const;
    Logger::Entry log() const;

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

    struct RequestOutcome {
        std::string response;
        bool close_after_response = false;
    };

    RequestOutcome make_response(
        int status_code,
        const std::string& reason,
        const std::string& cseq,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body,
        bool close_after_response = false) const;
    void start_read();
    void handle_read(asio::error_code ec, std::size_t bytes_read);
    void process_pending_requests();
    void queue_response(RequestOutcome outcome);
    void start_write();
    void handle_write(asio::error_code ec, std::size_t bytes_written);
    void close_socket();
    void finish();
    bool load_media_description(const std::filesystem::path& media_path, const std::string& media_uri);
    bool start_streaming();
    void stop_streaming();
    RequestOutcome handle_request(const std::string& raw_request);
    RequestOutcome handle_options(const std::string& cseq) const;
    RequestOutcome handle_describe(const std::string& uri, const std::string& cseq);
    RequestOutcome handle_setup(
        const std::string& uri,
        const std::string& cseq,
        const std::string& transport,
        const std::string& session_header);
    RequestOutcome handle_play(const std::string& cseq, const std::string& session_header);
    RequestOutcome handle_teardown(const std::string& cseq);
    std::string log_prefix() const;
    std::string session_id_text() const;
    bool has_setup_tracks() const;
    void reset_track_state();

    Socket socket_;
    std::string remote_endpoint_;
    std::filesystem::path media_dir_;
    std::uint32_t session_id_;
    CloseHandler on_close_;
    std::string current_media_uri_;
    TrackPorts video_ports_;
    TrackPorts audio_ports_;
    std::filesystem::path current_media_path_;
    MediaDescription current_media_;
    std::unique_ptr<MediaStreamer> streamer_;
    std::array<char, 4096> read_buffer_ {};
    std::string pending_requests_;
    std::deque<std::string> write_queue_;
    bool close_after_write_ = false;
    bool finished_ = false;
};
