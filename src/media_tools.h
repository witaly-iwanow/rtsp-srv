#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct MediaTrack {
    bool present = false;
    bool copy = false;
    int channels = 0;
    int sample_rate = 0;
    int stream_index = -1;
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
        std::filesystem::path media_path,
        MediaDescription media,
        StreamTarget video_target,
        StreamTarget audio_target,
        std::string rtp_cname,
        std::string log_prefix);

    ~MediaStreamer();

    MediaStreamer(const MediaStreamer&) = delete;
    MediaStreamer& operator=(const MediaStreamer&) = delete;

    bool start();
    void stop();
    bool running() const;

private:
    void run();
    void report_startup_result(bool ok, std::string error_text = {});

    std::filesystem::path media_path_;
    MediaDescription media_;
    StreamTarget video_target_;
    StreamTarget audio_target_;
    std::string rtp_cname_;
    std::string log_prefix_;
    std::thread worker_;
    std::atomic<bool> stop_requested_ {false};
    std::atomic<bool> running_ {false};
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool startup_done_ = false;
    bool startup_ok_ = false;
    std::string startup_error_;
};
