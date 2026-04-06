#pragma once

#include <asio.hpp>

#include <filesystem>
#include <memory>
#include <string>

struct MediaTrack {
    bool present = false;
    int channels = 0;
    int sample_rate = 0;
    int stream_index = -1;
    int rtp_payload_type = -1;
    std::string codec_name;
};

struct MediaDescription {
    MediaTrack video;
    MediaTrack audio;
    std::string sdp;
};

struct StreamTarget {
    bool enabled = false;
    std::string host;
    std::uint16_t client_rtp = 0;
    std::uint16_t client_rtcp = 0;
    std::uint16_t server_rtp = 0;
    std::uint16_t server_rtcp = 0;
};

bool describe_media(const std::filesystem::path& media_path, MediaDescription& media);

class MediaStreamer {
public:
    MediaStreamer(
        asio::any_io_executor executor,
        std::filesystem::path media_path,
        MediaDescription media,
        StreamTarget video_target,
        StreamTarget audio_target,
        std::string rtp_cname,
        std::string log_prefix);

    ~MediaStreamer();

    MediaStreamer(const MediaStreamer&) = delete;
    MediaStreamer& operator=(const MediaStreamer&) = delete;

    [[nodiscard]] bool start();
    void stop();
    [[nodiscard]] bool running() const;

private:
    void start_on_executor();
    void schedule_next_packet();
    void handle_timer(asio::error_code ec);
    void complete_startup(bool ok, std::string error_text = {});
    void finalize();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
