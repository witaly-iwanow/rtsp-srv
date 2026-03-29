#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

struct MediaTrack {
    bool present = false;
    bool copy = false;
    int channels = 0;
    int sample_rate = 0;
    std::string codec_name;
};

struct MediaDescription {
    MediaTrack video;
    MediaTrack audio;
    std::string sdp;
};

std::string join_command(const std::vector<std::string>& args);
pid_t spawn_process(const std::vector<std::string>& args, int stdout_fd = -1, int stderr_fd = -1);
bool wait_for_process(pid_t pid, const std::string& process_name, bool log_exit = true);
bool probe_track(const std::filesystem::path& media_path, const char* selector, bool is_video, MediaTrack& track);
std::vector<std::string> make_ffmpeg_args(
    const std::filesystem::path& media_path,
    const MediaDescription& media,
    const std::string* video_rtp_url,
    const std::string* audio_rtp_url,
    const std::string* rtp_cname,
    bool realtime,
    bool loop_input,
    const std::string* sdp_file = nullptr);
