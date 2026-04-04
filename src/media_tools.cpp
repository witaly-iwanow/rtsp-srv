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
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
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
    InputFormatContext() = default;
    InputFormatContext(const InputFormatContext&) = delete;
    InputFormatContext& operator=(const InputFormatContext&) = delete;

    ~InputFormatContext() {
        reset();
    }

    bool open(const std::filesystem::path& media_path, std::string& error_text) {
        reset();
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

    void reset() {
        if (context_ != nullptr)
            avformat_close_input(&context_);
    }

private:
    AVFormatContext* context_ = nullptr;
};

class OutputFormatContext {
public:
    OutputFormatContext() = default;
    OutputFormatContext(const OutputFormatContext&) = delete;
    OutputFormatContext& operator=(const OutputFormatContext&) = delete;

    ~OutputFormatContext() {
        reset();
    }

    void reset() {
        if (context_ == nullptr)
            return;

        if (header_written_)
            av_write_trailer(context_);
        if (!(context_->oformat->flags & AVFMT_NOFILE) && context_->pb != nullptr)
            avio_closep(&context_->pb);
        avformat_free_context(context_);
        context_ = nullptr;
        stream_ = nullptr;
        header_written_ = false;
    }

    bool init(
        const std::string& url,
        AVStream* input_stream,
        const std::string& rtp_cname,
        bool open_io,
        std::string& error_text) {
        reset();
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
    PacketHandle(const PacketHandle&) = delete;
    PacketHandle& operator=(const PacketHandle&) = delete;

    AVPacket* get() const {
        return packet_;
    }

    void reset() {
        if (packet_ != nullptr)
            av_packet_unref(packet_);
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
    std::int64_t loop_input_start_ts = AV_NOPTS_VALUE;
    std::int64_t loop_output_offset_ts = 0;
    std::int64_t last_output_end_ts = AV_NOPTS_VALUE;

    void reset() {
        input_index = -1;
        output.reset();
        loop_input_start_ts = AV_NOPTS_VALUE;
        loop_output_offset_ts = 0;
        last_output_end_ts = AV_NOPTS_VALUE;
    }

    void start_next_loop() {
        if (last_output_end_ts != AV_NOPTS_VALUE)
            loop_output_offset_ts = last_output_end_ts;
        loop_input_start_ts = AV_NOPTS_VALUE;
    }
};

void keep_timestamps_monotonic(AVPacket* packet, ActiveTrack& track) {
    const std::int64_t source_ts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    if (track.loop_input_start_ts == AV_NOPTS_VALUE && source_ts != AV_NOPTS_VALUE)
        track.loop_input_start_ts = source_ts;

    auto remap = [&](std::int64_t ts) {
        if (ts == AV_NOPTS_VALUE)
            return ts;
        const std::int64_t base = track.loop_input_start_ts == AV_NOPTS_VALUE ? ts : track.loop_input_start_ts;
        return ts - base + track.loop_output_offset_ts;
    };

    packet->pts = remap(packet->pts);
    packet->dts = remap(packet->dts);

    const std::int64_t out_ts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    if (out_ts == AV_NOPTS_VALUE)
        return;

    std::int64_t end_ts = out_ts + (packet->duration > 0 ? packet->duration : 1);
    if (track.last_output_end_ts == AV_NOPTS_VALUE || end_ts > track.last_output_end_ts)
        track.last_output_end_ts = end_ts;
}

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

struct MediaStreamer::Impl {
    explicit Impl(asio::any_io_executor executor): strand(asio::make_strand(std::move(executor))), timer(strand) {}

    asio::strand<asio::any_io_executor> strand;
    asio::steady_timer timer;
    std::filesystem::path media_path;
    MediaDescription media;
    StreamTarget video_target;
    StreamTarget audio_target;
    std::string rtp_cname;
    std::string log_prefix;
    InputFormatContext input;
    ActiveTrack video;
    ActiveTrack audio;
    PacketHandle packet;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::atomic<bool> running {false};
    bool stop_requested = false;
    bool startup_done = false;
    bool startup_ok = false;
    bool startup_reported = false;
    bool stop_done = true;
    std::string startup_error;
    std::int64_t wall_start_us = AV_NOPTS_VALUE;
    std::int64_t media_start_us = AV_NOPTS_VALUE;
};

MediaStreamer::MediaStreamer(
    asio::any_io_executor executor,
    std::filesystem::path media_path,
    MediaDescription media,
    StreamTarget video_target,
    StreamTarget audio_target,
    std::string rtp_cname,
    std::string log_prefix):
    impl_(std::make_unique<Impl>(std::move(executor))) {
    impl_->media_path = std::move(media_path);
    impl_->media = std::move(media);
    impl_->video_target = std::move(video_target);
    impl_->audio_target = std::move(audio_target);
    impl_->rtp_cname = std::move(rtp_cname);
    impl_->log_prefix = std::move(log_prefix);
}

MediaStreamer::~MediaStreamer() {
    stop();
}

bool MediaStreamer::start() {
    if (impl_->running.load())
        return running();

    {
        const std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->stop_requested = false;
        impl_->startup_done = false;
        impl_->startup_ok = false;
        impl_->startup_reported = false;
        impl_->stop_done = false;
        impl_->startup_error.clear();
    }
    impl_->running.store(false);
    asio::post(impl_->strand, [this]() {
        start_on_executor();
    });

    std::unique_lock<std::mutex> lock(impl_->state_mutex);
    impl_->state_cv.wait(lock, [this]() {
        return impl_->startup_done;
    });

    if (impl_->startup_ok)
        return true;

    if (!impl_->startup_error.empty())
        LOG << impl_->log_prefix << " media pipeline startup failed: " << impl_->startup_error;
    return false;
}

void MediaStreamer::stop() {
    {
        const std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->stop_done)
            return;
        impl_->stop_requested = true;
    }
    asio::post(impl_->strand, [this]() {
        impl_->timer.cancel();
        finalize();
    });

    std::unique_lock<std::mutex> lock(impl_->state_mutex);
    impl_->state_cv.wait(lock, [this]() {
        return impl_->stop_done;
    });
}

bool MediaStreamer::running() const {
    return impl_->running.load();
}

void MediaStreamer::complete_startup(bool ok, std::string error_text) {
    {
        const std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->startup_reported)
            return;
        impl_->startup_done = true;
        impl_->startup_ok = ok;
        impl_->startup_reported = true;
        impl_->startup_error = std::move(error_text);
    }
    impl_->state_cv.notify_all();
}

void MediaStreamer::start_on_executor() {
    std::string error_text;
    if (!impl_->input.open(impl_->media_path, error_text)) {
        complete_startup(false, std::move(error_text));
        finalize();
        return;
    }

    if (impl_->video_target.enabled) {
        impl_->video.input_index = impl_->media.video.stream_index;
        if (!impl_->video.output.init(
                make_rtp_url(impl_->video_target),
                impl_->input.get()->streams[impl_->video.input_index],
                impl_->rtp_cname,
                true,
                error_text)) {
            complete_startup(false, std::move(error_text));
            finalize();
            return;
        }
    }

    if (impl_->audio_target.enabled) {
        impl_->audio.input_index = impl_->media.audio.stream_index;
        if (!impl_->audio.output.init(
                make_rtp_url(impl_->audio_target),
                impl_->input.get()->streams[impl_->audio.input_index],
                impl_->rtp_cname,
                true,
                error_text)) {
            complete_startup(false, std::move(error_text));
            finalize();
            return;
        }
    }

    if (impl_->packet.get() == nullptr) {
        complete_startup(false, "av_packet_alloc failed");
        finalize();
        return;
    }

    impl_->running.store(true);
    impl_->wall_start_us = AV_NOPTS_VALUE;
    impl_->media_start_us = AV_NOPTS_VALUE;
    complete_startup(true);
    schedule_next_packet();
}

void MediaStreamer::schedule_next_packet() {
    if (impl_->stop_requested) {
        finalize();
        return;
    }

    while (!impl_->stop_requested) {
        AVPacket* current = impl_->packet.get();
        int err = av_read_frame(impl_->input.get(), current);
        if (err == AVERROR_EOF) {
            std::string error_text;
            if (!rewind_input(impl_->input.get(), error_text)) {
                LOG << impl_->log_prefix << " rewind failed: " << error_text;
                finalize();
                return;
            }
            impl_->video.start_next_loop();
            impl_->audio.start_next_loop();
            impl_->wall_start_us = AV_NOPTS_VALUE;
            impl_->media_start_us = AV_NOPTS_VALUE;
            continue;
        }
        if (err < 0) {
            LOG << impl_->log_prefix << " av_read_frame failed: " << ffmpeg_error_text(err);
            finalize();
            return;
        }

        ActiveTrack* output = nullptr;
        if (impl_->video_target.enabled && current->stream_index == impl_->video.input_index)
            output = &impl_->video;
        else if (impl_->audio_target.enabled && current->stream_index == impl_->audio.input_index)
            output = &impl_->audio;

        if (output == nullptr) {
            impl_->packet.reset();
            continue;
        }

        AVStream* input_stream = impl_->input.get()->streams[current->stream_index];
        const std::int64_t ts = current->pts != AV_NOPTS_VALUE ? current->pts : current->dts;
        if (ts != AV_NOPTS_VALUE) {
            const std::int64_t packet_time_us = av_rescale_q(ts, input_stream->time_base, AV_TIME_BASE_Q);
            if (impl_->media_start_us == AV_NOPTS_VALUE) {
                impl_->media_start_us = packet_time_us;
                impl_->wall_start_us = av_gettime_relative();
            }

            const std::int64_t target_time_us = impl_->wall_start_us + (packet_time_us - impl_->media_start_us);
            const std::int64_t delay_us = target_time_us - av_gettime_relative();
            if (delay_us > 0) {
                impl_->timer.expires_after(std::chrono::microseconds(delay_us));
                impl_->timer.async_wait(asio::bind_executor(impl_->strand, [this](asio::error_code ec) {
                    handle_timer(ec);
                }));
                return;
            }
        }

        av_packet_rescale_ts(current, input_stream->time_base, output->output.stream()->time_base);
        keep_timestamps_monotonic(current, *output);
        current->stream_index = 0;
        err = av_interleaved_write_frame(output->output.get(), current);
        impl_->packet.reset();
        if (err < 0) {
            LOG << impl_->log_prefix << " av_interleaved_write_frame failed: " << ffmpeg_error_text(err);
            finalize();
            return;
        }
    }
}

void MediaStreamer::handle_timer(asio::error_code ec) {
    if (ec == asio::error::operation_aborted) {
        if (impl_->stop_requested)
            finalize();
        return;
    }
    if (ec) {
        LOG << impl_->log_prefix << " timer failed: " << ec.message();
        finalize();
        return;
    }

    if (impl_->stop_requested) {
        finalize();
        return;
    }

    AVPacket* current = impl_->packet.get();
    ActiveTrack* output = nullptr;
    if (impl_->video_target.enabled && current->stream_index == impl_->video.input_index)
        output = &impl_->video;
    else if (impl_->audio_target.enabled && current->stream_index == impl_->audio.input_index)
        output = &impl_->audio;

    if (output != nullptr) {
        AVStream* input_stream = impl_->input.get()->streams[current->stream_index];
        av_packet_rescale_ts(current, input_stream->time_base, output->output.stream()->time_base);
        keep_timestamps_monotonic(current, *output);
        current->stream_index = 0;
        const int err = av_interleaved_write_frame(output->output.get(), current);
        impl_->packet.reset();
        if (err < 0) {
            LOG << impl_->log_prefix << " av_interleaved_write_frame failed: " << ffmpeg_error_text(err);
            finalize();
            return;
        }
    } else {
        impl_->packet.reset();
    }

    schedule_next_packet();
}

void MediaStreamer::finalize() {
    if (impl_->running.exchange(false)) {
        impl_->timer.cancel();
    }
    impl_->video.reset();
    impl_->audio.reset();
    impl_->input.reset();
    impl_->packet.reset();

    {
        const std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->stop_done = true;
        if (!impl_->startup_reported) {
            impl_->startup_done = true;
            impl_->startup_ok = false;
            impl_->startup_reported = true;
        }
    }
    impl_->state_cv.notify_all();
}
