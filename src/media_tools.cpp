#include "media_tools.h"
#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t kDescribeVideoRtpPort = 40000;
constexpr std::uint16_t kDescribeAudioRtpPort = 40002;

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return value;
}

bool is_passthrough_video_codec(AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_HEVC || codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_MPEG2VIDEO
        || codec_id == AV_CODEC_ID_VP8 || codec_id == AV_CODEC_ID_VP9;
}

bool is_passthrough_audio_codec(AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_OPUS || codec_id == AV_CODEC_ID_AAC || codec_id == AV_CODEC_ID_MP1
        || codec_id == AV_CODEC_ID_MP2 || codec_id == AV_CODEC_ID_MP3;
}

std::string ffmpeg_error_text(int errnum) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer {};
    av_strerror(errnum, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

void ensure_network_initialized() {
    static const int kInitOnce = []() {
        av_log_set_level(AV_LOG_ERROR);
        avformat_network_init();
        return 0;
    }();
    static_cast<void>(kInitOnce);
}

class InputFormatContext {
public:
    ~InputFormatContext() {
        if (context_ != nullptr)
            avformat_close_input(&context_);
    }

    bool open(const std::filesystem::path& media_path, std::string& error_text) {
        ensure_network_initialized();
        int err = avformat_open_input(&context_, media_path.c_str(), nullptr, nullptr);
        if (err < 0) {
            error_text = "avformat_open_input failed: " + ffmpeg_error_text(err);
            return false;
        }

        err = avformat_find_stream_info(context_, nullptr);
        if (err < 0) {
            error_text = "avformat_find_stream_info failed: " + ffmpeg_error_text(err);
            return false;
        }
        return true;
    }

    AVFormatContext* get() const {
        return context_;
    }

private:
    AVFormatContext* context_ = nullptr;
};

class OutputFormatContext {
public:
    ~OutputFormatContext() {
        if (context_ == nullptr)
            return;

        if (header_written_)
            av_write_trailer(context_);
        if (!(context_->oformat->flags & AVFMT_NOFILE) && context_->pb != nullptr)
            avio_closep(&context_->pb);
        avformat_free_context(context_);
    }

    bool init(
        const std::string& url,
        AVStream* input_stream,
        const std::string& rtp_cname,
        bool open_io,
        std::string& error_text) {
        int err = avformat_alloc_output_context2(&context_, nullptr, "rtp", url.c_str());
        if (err < 0 || context_ == nullptr) {
            error_text = "avformat_alloc_output_context2 failed: " + ffmpeg_error_text(err);
            return false;
        }

        stream_ = avformat_new_stream(context_, nullptr);
        if (stream_ == nullptr) {
            error_text = "avformat_new_stream failed";
            return false;
        }

        err = avcodec_parameters_copy(stream_->codecpar, input_stream->codecpar);
        if (err < 0) {
            error_text = "avcodec_parameters_copy failed: " + ffmpeg_error_text(err);
            return false;
        }
        stream_->codecpar->codec_tag = 0;
        stream_->time_base = input_stream->time_base;
        stream_->avg_frame_rate = input_stream->avg_frame_rate;

        if (!open_io)
            return true;

        if (!(context_->oformat->flags & AVFMT_NOFILE)) {
            err = avio_open2(&context_->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (err < 0) {
                error_text = "avio_open2 failed: " + ffmpeg_error_text(err);
                return false;
            }
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "cname", rtp_cname.c_str(), 0);
        err = avformat_write_header(context_, &options);
        av_dict_free(&options);
        if (err < 0) {
            error_text = "avformat_write_header failed: " + ffmpeg_error_text(err);
            return false;
        }
        header_written_ = true;
        return true;
    }

    AVFormatContext* get() const {
        return context_;
    }

    AVStream* stream() const {
        return stream_;
    }

private:
    AVFormatContext* context_ = nullptr;
    AVStream* stream_ = nullptr;
    bool header_written_ = false;
};

class PacketHandle {
public:
    PacketHandle(): packet_(av_packet_alloc()) {}

    AVPacket* get() const {
        return packet_;
    }

    ~PacketHandle() {
        if (packet_ != nullptr)
            av_packet_free(&packet_);
    }

private:
    AVPacket* packet_ = nullptr;
};

struct ActiveTrack {
    int input_index = -1;
    OutputFormatContext output;
};

std::string make_rtp_url(const StreamTarget& target) {
    std::ostringstream url;
    if (target.host.find(':') != std::string::npos) {
        url << "rtp://[" << target.host << "]:" << target.client_rtp;
    } else {
        url << "rtp://" << target.host << ":" << target.client_rtp;
    }
    url << "?pkt_size=1200"
        << "&rtcpport=" << target.client_rtcp
        << "&localrtpport=" << target.server_rtp
        << "&localrtcpport=" << target.server_rtcp;
    return url.str();
}

std::string normalize_sdp(const std::string& raw_sdp, const std::string& media_name) {
    std::istringstream iss(raw_sdp);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(line);
    }

    std::vector<std::string> normalized;
    normalized.reserve(lines.size() + 4);
    bool has_session_control = false;
    bool has_time = false;
    int track_id = -1;
    for (const std::string& original: lines) {
        if (original.rfind("a=control:", 0) == 0)
            continue;

        std::string current = original;
        if (current.rfind("s=", 0) == 0)
            current = "s=" + media_name;
        else if (current.rfind("c=", 0) == 0)
            current = "c=IN IP4 0.0.0.0";
        else if (current.rfind("m=", 0) == 0) {
            if (track_id >= 0)
                normalized.emplace_back("a=control:trackID=" + std::to_string(track_id));
            ++track_id;

            std::istringstream media_line(current);
            std::string prefix;
            std::string media_type;
            std::string port;
            std::string proto;
            if ((media_line >> prefix >> port >> proto))
                current = prefix + " 0 " + proto + current.substr(prefix.size() + 1 + port.size() + 1 + proto.size());
        } else if (current.rfind("t=", 0) == 0) {
            has_time = true;
        }

        normalized.push_back(current);
        if (current.rfind("t=", 0) == 0 && !has_session_control) {
            normalized.emplace_back("a=control:*");
            has_session_control = true;
        }
    }

    if (!has_time) {
        normalized.emplace_back("t=0 0");
        normalized.emplace_back("a=control:*");
        has_session_control = true;
    }
    if (!has_session_control)
        normalized.emplace_back("a=control:*");
    if (track_id >= 0)
        normalized.emplace_back("a=control:trackID=" + std::to_string(track_id));

    std::ostringstream sdp;
    for (const std::string& normalized_line: normalized)
        sdp << normalized_line << "\r\n";
    return sdp.str();
}

bool fill_track_description(AVStream* stream, bool is_video, MediaTrack& track) {
    if (stream == nullptr || stream->codecpar == nullptr)
        return false;

    track.present = true;
    track.stream_index = stream->index;
    track.codec_name = to_lower(avcodec_get_name(stream->codecpar->codec_id));
    track.channels = stream->codecpar->ch_layout.nb_channels;
    track.sample_rate = stream->codecpar->sample_rate;
    track.copy = is_video ? is_passthrough_video_codec(stream->codecpar->codec_id)
                          : is_passthrough_audio_codec(stream->codecpar->codec_id);
    return track.copy;
}

bool build_sdp(
    AVFormatContext* input,
    const MediaDescription& media,
    const std::string& media_name,
    std::string& sdp_out,
    std::string& error_text) {
    std::vector<std::unique_ptr<OutputFormatContext>> outputs;
    std::vector<AVFormatContext*> contexts;

    auto add_track = [&](const MediaTrack& track, std::uint16_t port) -> bool {
        if (!track.present)
            return true;

        const std::string host = "127.0.0.1";
        StreamTarget target;
        target.enabled = true;
        target.host = host;
        target.client_rtp = port;
        target.client_rtcp = static_cast<std::uint16_t>(port + 1);
        target.server_rtp = port;
        target.server_rtcp = static_cast<std::uint16_t>(port + 1);

        auto output = std::make_unique<OutputFormatContext>();
        if (!output->init(make_rtp_url(target), input->streams[track.stream_index], "rtsp-sdp", false, error_text))
            return false;

        contexts.push_back(output->get());
        outputs.push_back(std::move(output));
        return true;
    };

    if (!add_track(media.video, kDescribeVideoRtpPort) || !add_track(media.audio, kDescribeAudioRtpPort))
        return false;
    if (contexts.empty()) {
        error_text = "no supported RTP tracks found";
        return false;
    }

    std::vector<char> buffer(16384, '\0');
    const int err = av_sdp_create(contexts.data(), static_cast<int>(contexts.size()), buffer.data(), buffer.size());
    if (err < 0) {
        error_text = "av_sdp_create failed: " + ffmpeg_error_text(err);
        return false;
    }

    sdp_out = normalize_sdp(std::string(buffer.data()), media_name);
    return !sdp_out.empty();
}

bool wait_for_media_time(
    std::condition_variable& state_cv,
    std::mutex& state_mutex,
    const std::atomic<bool>& stop_requested,
    std::int64_t delay_us) {
    if (delay_us <= 0)
        return true;

    std::unique_lock<std::mutex> lock(state_mutex);
    return !state_cv.wait_for(lock, std::chrono::microseconds(delay_us), [&stop_requested]() {
        return stop_requested.load();
    });
}

bool rewind_input(AVFormatContext* input, std::string& error_text) {
    const int err = av_seek_frame(input, -1, 0, AVSEEK_FLAG_BACKWARD);
    if (err < 0) {
        error_text = "av_seek_frame failed: " + ffmpeg_error_text(err);
        return false;
    }
    avformat_flush(input);
    return true;
}

}  // namespace

bool describe_media(const std::filesystem::path& media_path, MediaDescription& media) {
    InputFormatContext input;
    std::string error_text;
    if (!input.open(media_path, error_text)) {
        LOG << "Failed to inspect media " << media_path << ": " << error_text;
        return false;
    }

    media = {};
    for (unsigned int i = 0; i < input.get()->nb_streams; ++i) {
        AVStream* stream = input.get()->streams[i];
        if (stream->codecpar == nullptr)
            continue;

        if (!media.video.present && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (!fill_track_description(stream, true, media.video)) {
                LOG << "Unsupported video codec " << avcodec_get_name(stream->codecpar->codec_id)
                    << " in " << media_path;
                return false;
            }
            continue;
        }

        if (!media.audio.present && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!fill_track_description(stream, false, media.audio)) {
                LOG << "Unsupported audio codec " << avcodec_get_name(stream->codecpar->codec_id)
                    << " in " << media_path;
                return false;
            }
        }
    }

    if (!media.video.present && !media.audio.present) {
        LOG << "No supported media tracks found in " << media_path;
        return false;
    }

    if (!build_sdp(input.get(), media, media_path.filename().string(), media.sdp, error_text)) {
        LOG << "Failed to build SDP for " << media_path << ": " << error_text;
        return false;
    }

    return true;
}

MediaStreamer::MediaStreamer(
    std::filesystem::path media_path,
    MediaDescription media,
    StreamTarget video_target,
    StreamTarget audio_target,
    std::string rtp_cname,
    std::string log_prefix):
    media_path_(std::move(media_path)),
    media_(std::move(media)),
    video_target_(std::move(video_target)),
    audio_target_(std::move(audio_target)),
    rtp_cname_(std::move(rtp_cname)),
    log_prefix_(std::move(log_prefix)) {}

MediaStreamer::~MediaStreamer() {
    stop();
}

bool MediaStreamer::start() {
    if (worker_.joinable())
        return running();

    stop_requested_.store(false);
    running_.store(false);
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        startup_done_ = false;
        startup_ok_ = false;
        startup_error_.clear();
    }

    worker_ = std::thread([this]() {
        run();
    });

    std::unique_lock<std::mutex> lock(state_mutex_);
    state_cv_.wait(lock, [this]() {
        return startup_done_;
    });

    if (startup_ok_)
        return true;

    lock.unlock();
    if (worker_.joinable())
        worker_.join();
    if (!startup_error_.empty())
        LOG << log_prefix_ << " media pipeline startup failed: " << startup_error_;
    return false;
}

void MediaStreamer::stop() {
    stop_requested_.store(true);
    state_cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    running_.store(false);
}

bool MediaStreamer::running() const {
    return running_.load();
}

void MediaStreamer::report_startup_result(bool ok, std::string error_text) {
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        startup_done_ = true;
        startup_ok_ = ok;
        startup_error_ = std::move(error_text);
    }
    state_cv_.notify_all();
}

void MediaStreamer::run() {
    InputFormatContext input;
    std::string error_text;
    if (!input.open(media_path_, error_text)) {
        report_startup_result(false, std::move(error_text));
        return;
    }

    ActiveTrack video;
    ActiveTrack audio;

    if (video_target_.enabled) {
        video.input_index = media_.video.stream_index;
        if (!video.output.init(make_rtp_url(video_target_), input.get()->streams[video.input_index], rtp_cname_, true, error_text)) {
            report_startup_result(false, std::move(error_text));
            return;
        }
    }

    if (audio_target_.enabled) {
        audio.input_index = media_.audio.stream_index;
        if (!audio.output.init(make_rtp_url(audio_target_), input.get()->streams[audio.input_index], rtp_cname_, true, error_text)) {
            report_startup_result(false, std::move(error_text));
            return;
        }
    }

    PacketHandle packet;
    if (packet.get() == nullptr) {
        report_startup_result(false, "av_packet_alloc failed");
        return;
    }

    running_.store(true);
    report_startup_result(true);

    std::int64_t wall_start_us = AV_NOPTS_VALUE;
    std::int64_t media_start_us = AV_NOPTS_VALUE;

    while (!stop_requested_.load()) {
        AVPacket* current = packet.get();
        int err = av_read_frame(input.get(), current);
        if (err == AVERROR_EOF) {
            if (!rewind_input(input.get(), error_text)) {
                LOG << log_prefix_ << " rewind failed: " << error_text;
                break;
            }
            wall_start_us = AV_NOPTS_VALUE;
            media_start_us = AV_NOPTS_VALUE;
            continue;
        }
        if (err < 0) {
            LOG << log_prefix_ << " av_read_frame failed: " << ffmpeg_error_text(err);
            break;
        }

        ActiveTrack* output = nullptr;
        if (video_target_.enabled && current->stream_index == video.input_index)
            output = &video;
        else if (audio_target_.enabled && current->stream_index == audio.input_index)
            output = &audio;

        if (output == nullptr) {
            av_packet_unref(current);
            continue;
        }

        AVStream* input_stream = input.get()->streams[current->stream_index];
        const std::int64_t ts = current->pts != AV_NOPTS_VALUE ? current->pts : current->dts;
        if (ts != AV_NOPTS_VALUE) {
            const std::int64_t packet_time_us = av_rescale_q(ts, input_stream->time_base, AV_TIME_BASE_Q);
            if (media_start_us == AV_NOPTS_VALUE) {
                media_start_us = packet_time_us;
                wall_start_us = av_gettime_relative();
            } else {
                const std::int64_t target_time_us = wall_start_us + (packet_time_us - media_start_us);
                const std::int64_t delay_us = target_time_us - av_gettime_relative();
                if (!wait_for_media_time(state_cv_, state_mutex_, stop_requested_, delay_us)) {
                    av_packet_unref(current);
                    break;
                }
            }
        }

        av_packet_rescale_ts(current, input_stream->time_base, output->output.stream()->time_base);
        current->stream_index = 0;
        err = av_interleaved_write_frame(output->output.get(), current);
        av_packet_unref(current);
        if (err < 0) {
            LOG << log_prefix_ << " av_interleaved_write_frame failed: " << ffmpeg_error_text(err);
            break;
        }
    }

    running_.store(false);
}
