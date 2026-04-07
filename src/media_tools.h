#pragma once

#include <asio.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// Describes one track (video or audio) found in a media file.
struct MediaTrack {
    bool present = false;
    int channels = 0;
    int sample_rate = 0;
    int stream_index = -1;
    int rtp_payload_type = -1;
    std::string codec_name;
};

// Full media description with video/audio tracks and a generated SDP for the client.
struct MediaDescription {
    MediaTrack video;
    MediaTrack audio;
    std::string sdp;
};

// Destination addresses and ports for one RTP track sent to a client.
struct StreamTarget {
    bool enabled = false;
    std::string host;
    std::uint16_t client_rtp = 0;
    std::uint16_t client_rtcp = 0;
    std::uint16_t server_rtp = 0;
    std::uint16_t server_rtcp = 0;
};

bool describe_media(const std::filesystem::path& media_path, MediaDescription& media);

// Inspects a media file and streams selected tracks over RTP using FFmpeg's RTP muxer.
// Runs on a dedicated executor (strand) so the calling RTSP thread is not blocked after startup.
class MediaStreamer {
public:
    using StartHandler = std::function<void(bool, std::string)>;
    using StopHandler = std::function<void()>;

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

    void start(StartHandler handler);
    void stop(StopHandler handler = {});
    [[nodiscard]] bool running() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
