#pragma once

#include <asio.hpp>

#include "logger.h"
#include "media_tools.h"

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Handles one RTSP client connection: parses requests, manages media setup,
// and owns the MediaStreamer that pushes RTP packets to the client.
class Session : public std::enable_shared_from_this<Session> {
public:
    using Ptr = std::shared_ptr<Session>;
    using Socket = asio::ip::tcp::socket;
    using MediaExecutor = asio::any_io_executor;
    using CloseHandler = std::function<void(std::uint32_t, const std::string&)>;

    Session(Socket socket, std::string remote_endpoint, const std::filesystem::path& media_dir, MediaExecutor media_executor, std::uint32_t session_id, CloseHandler on_close);

    ~Session();

    void start();
    void shutdown();
    const std::string& remote_endpoint() const;
    std::uint32_t session_id() const;
    Logger::Entry log() const;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:
    // Per-track port assignments negotiated during SETUP.
    struct TrackPorts {
        std::uint16_t client_rtp = 0;
        std::uint16_t client_rtcp = 0;
        std::uint16_t server_rtp = 0;
        std::uint16_t server_rtcp = 0;
        bool setup = false;
    };

    // Result of processing one RTSP request: the response text and whether to close the connection.
    struct RequestOutcome {
        std::string response;
        bool close_after_response = false;
    };

    enum class StreamState {
        Idle,
        Starting,
        Playing,
        Stopping
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
    void start_playback(std::string cseq);
    void handle_playback_started(bool ok, std::string cseq, std::string error_text);
    void stop_playback(std::optional<RequestOutcome> response, bool finish_after_stop);
    void handle_playback_stopped();
    void queue_response(RequestOutcome outcome);
    void start_write();
    void handle_write(asio::error_code ec, std::size_t bytes_written);
    void close_socket();
    void finalize_close();
    void finish();
    [[nodiscard]] bool load_media_description(const std::filesystem::path& media_path, const std::string& media_uri);
    [[nodiscard]] bool create_streamer();
    std::optional<RequestOutcome> handle_request(const std::string& raw_request);
    RequestOutcome handle_options(const std::string& cseq) const;
    RequestOutcome handle_describe(const std::string& uri, const std::string& cseq);
    RequestOutcome handle_setup(const std::string& uri, const std::string& cseq, const std::string& transport, const std::string& session_header);
    std::optional<RequestOutcome> handle_play(const std::string& cseq, const std::string& session_header);
    std::optional<RequestOutcome> handle_teardown(const std::string& cseq);
    std::string log_prefix() const;
    std::string session_id_text() const;
    bool has_setup_tracks() const;
    void reset_track_state();

    Socket socket_;
    std::string remote_endpoint_;
    std::filesystem::path media_dir_;
    MediaExecutor media_executor_;
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
    StreamState stream_state_ = StreamState::Idle;
    std::optional<RequestOutcome> pending_stop_response_;
    bool finish_after_stop_ = false;
};
