#include "media_tools.h"
#include "logger.h"
#include "utils.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#include <array>
#include <chrono>
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

bool is_supported_video_codec(AVCodecID codec_id) {
    return codec_id == AV_CODEC_ID_HEVC || codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_MPEG2VIDEO
        || codec_id == AV_CODEC_ID_VP8 || codec_id == AV_CODEC_ID_VP9;
}

bool is_supported_audio_codec(AVCodecID codec_id) {
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

// RAII wrapper around AVFormatContext for reading input files.
class InputFormatContext {
public:
    InputFormatContext() = default;
    InputFormatContext(const InputFormatContext&) = delete;
    InputFormatContext& operator=(const InputFormatContext&) = delete;

    virtual ~InputFormatContext() {
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
        if (context_) {
            avformat_close_input(&context_);
            context_ = nullptr;
        }
    }

private:
    AVFormatContext* context_ = nullptr;
};

// RAII wrapper around an AVFormatContext used as an RTP output muxer.
class OutputFormatContext {
public:
    OutputFormatContext() = default;
    OutputFormatContext(const OutputFormatContext&) = delete;
    OutputFormatContext& operator=(const OutputFormatContext&) = delete;

    virtual ~OutputFormatContext() {
        reset();
    }

    void reset() {
        if (!context_)
            return;

        if (header_written_)
            av_write_trailer(context_);
        if (!(context_->oformat->flags & AVFMT_NOFILE) && context_->pb)
            avio_closep(&context_->pb);

        avformat_free_context(context_);
        context_ = nullptr;
        stream_ = nullptr;
        header_written_ = false;
    }

    bool init(const std::string& url, AVStream* input_stream, const std::string& rtp_cname, bool open_io, int payload_type, std::string& error_text) {
        reset();
        int err = avformat_alloc_output_context2(&context_, nullptr, "rtp", url.c_str());
        if (err < 0 || context_ == nullptr) {
            error_text = "avformat_alloc_output_context2 failed: " + ffmpeg_error_text(err);
            return false;
        }

        stream_ = avformat_new_stream(context_, nullptr);
        if (!stream_) {
            error_text = "avformat_new_stream failed";
            reset();
            return false;
        }

        err = avcodec_parameters_copy(stream_->codecpar, input_stream->codecpar);
        if (err < 0) {
            error_text = "avcodec_parameters_copy failed: " + ffmpeg_error_text(err);
            reset();
            return false;
        }
        if (payload_type >= 0 && payload_type <= 127)
            stream_->id = payload_type;
        stream_->codecpar->codec_tag = 0;
        stream_->time_base = input_stream->time_base;
        stream_->avg_frame_rate = input_stream->avg_frame_rate;

        if (!open_io)
            return true;

        if (!(context_->oformat->flags & AVFMT_NOFILE)) {
            err = avio_open2(&context_->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (err < 0) {
                error_text = "avio_open2 failed: " + ffmpeg_error_text(err);
                reset();
                return false;
            }
        }

        AVDictionary* options = nullptr;
        av_dict_set(&options, "cname", rtp_cname.c_str(), 0);
        std::string payload_type_text;
        if (payload_type >= 96 && payload_type <= 127) {
            payload_type_text = std::to_string(payload_type);
            av_dict_set(&options, "payload_type", payload_type_text.c_str(), 0);
        }
        err = avformat_write_header(context_, &options);
        av_dict_free(&options);
        if (err < 0) {
            error_text = "avformat_write_header failed: " + ffmpeg_error_text(err);
            reset();
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

// RAII wrapper around AVPacket that allocates the packet once and unrefs it between uses.
class PacketHandle {
public:
    PacketHandle(): packet_(av_packet_alloc()) {}
    PacketHandle(const PacketHandle&) = delete;
    PacketHandle& operator=(const PacketHandle&) = delete;

    AVPacket* get() const {
        return packet_;
    }

    void unref() {
        av_packet_unref(packet_);
    }

    virtual ~PacketHandle() {
        av_packet_free(&packet_);
    }

private:
    AVPacket* packet_ = nullptr;
};

// RAII wrapper for the FFmpeg bitstream filter we use to derive AAC extradata from ADTS packets.
class BitstreamFilterContext {
public:
    BitstreamFilterContext() = default;
    BitstreamFilterContext(const BitstreamFilterContext&) = delete;
    BitstreamFilterContext& operator=(const BitstreamFilterContext&) = delete;

    virtual ~BitstreamFilterContext() {
        reset();
    }

    AVBSFContext* get() const {
        return context_;
    }

    bool init(const char* filter_name, AVStream* stream, std::string& error_text) {
        reset();

        const AVBitStreamFilter* filter = av_bsf_get_by_name(filter_name);
        if (filter == nullptr) {
            error_text = std::string("bitstream filter not available: ") + filter_name;
            return false;
        }

        int err = av_bsf_alloc(filter, &context_);
        if (err < 0) {
            error_text = "av_bsf_alloc failed: " + ffmpeg_error_text(err);
            return false;
        }

        err = avcodec_parameters_copy(context_->par_in, stream->codecpar);
        if (err < 0) {
            error_text = "avcodec_parameters_copy failed: " + ffmpeg_error_text(err);
            reset();
            return false;
        }
        context_->time_base_in = stream->time_base;

        err = av_bsf_init(context_);
        if (err < 0) {
            error_text = "av_bsf_init failed: " + ffmpeg_error_text(err);
            reset();
            return false;
        }

        return true;
    }

    void reset() {
        if (context_) {
            av_bsf_free(&context_);
            context_ = nullptr;
        }
    }

private:
    AVBSFContext* context_ = nullptr;
};

// Tracks per-track state for continuous looping: output muxer and timestamp remap offsets.
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

std::string normalize_sdp(const std::string& raw_sdp) {
    std::istringstream iss(raw_sdp);
    std::vector<std::string> cleaned;
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line.rfind("a=control:", 0) == 0)
            continue;
        cleaned.push_back(line);
    }

    int media_count = 0;
    for (const std::string& cleaned_line: cleaned)
        if (cleaned_line.rfind("m=", 0) == 0)
            ++media_count;

    // RTSP clients need explicit session and track control attributes.
    std::vector<std::string> normalized;
    normalized.reserve(cleaned.size() + 4 + static_cast<std::size_t>(media_count));
    bool has_session_control = false;
    bool has_time = false;
    int track_id = -1;
    for (const std::string& cleaned_line: cleaned) {
        if (cleaned_line.rfind("m=", 0) == 0) {
            if (track_id >= 0)
                normalized.emplace_back("a=control:trackID=" + std::to_string(track_id));
            ++track_id;
        } else if (cleaned_line.rfind("t=", 0) == 0) {
            has_time = true;
        }

        normalized.push_back(cleaned_line);
        if (cleaned_line.rfind("t=", 0) == 0 && !has_session_control) {
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

void remember_payload_types_from_sdp(const std::string& sdp, MediaDescription& media) {
    media.video.rtp_payload_type = -1;
    media.audio.rtp_payload_type = -1;

    std::istringstream iss(sdp);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("m=", 0) != 0)
            continue;

        std::istringstream ls(line.substr(2));
        std::string media_type;
        std::string port;
        std::string proto;
        std::string payload_text;
        if (!(ls >> media_type >> port >> proto >> payload_text))
            continue;

        int payload_type = -1;
        if (!util::parse_int(payload_text, payload_type))
            continue;

        if (media_type == "video" && media.video.present && media.video.rtp_payload_type < 0)
            media.video.rtp_payload_type = payload_type;
        else if (media_type == "audio" && media.audio.present && media.audio.rtp_payload_type < 0)
            media.audio.rtp_payload_type = payload_type;
    }
}

bool fill_track_description(AVStream* stream, bool is_video, MediaTrack& track) {
    if (stream == nullptr || stream->codecpar == nullptr)
        return false;

    const AVCodecID codec_id = stream->codecpar->codec_id;
    const bool supported = is_video ? is_supported_video_codec(codec_id) : is_supported_audio_codec(codec_id);
    if (!supported)
        return false;

    track = {};
    track.present = true;
    track.stream_index = stream->index;
    track.codec_name = util::to_lower(avcodec_get_name(codec_id));
    track.channels = stream->codecpar->ch_layout.nb_channels;
    track.sample_rate = stream->codecpar->sample_rate;
    return true;
}

bool build_sdp(AVFormatContext* input, MediaDescription& media, std::string& sdp_out, std::string& error_text) {
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
        if (!output->init(make_rtp_url(target), input->streams[track.stream_index], "rtsp-sdp", false, -1, error_text))
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

    sdp_out = normalize_sdp(std::string(buffer.data()));
    remember_payload_types_from_sdp(sdp_out, media);
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

bool copy_codec_extradata(AVCodecParameters* codecpar, const std::uint8_t* data, int size, std::string& error_text) {
    if (codecpar == nullptr || data == nullptr || size <= 0) {
        error_text = "invalid codec extradata";
        return false;
    }

    auto* extradata = static_cast<std::uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (extradata == nullptr) {
        error_text = "av_mallocz failed for codec extradata";
        return false;
    }

    std::memcpy(extradata, data, static_cast<std::size_t>(size));
    av_freep(&codecpar->extradata);
    codecpar->extradata = extradata;
    codecpar->extradata_size = size;
    return true;
}

bool derive_aac_audio_specific_config(AVFormatContext* input, AVStream* stream, std::string& error_text) {
    if (stream == nullptr || stream->codecpar == nullptr || stream->codecpar->codec_id != AV_CODEC_ID_AAC)
        return true;
    if (stream->codecpar->extradata != nullptr && stream->codecpar->extradata_size > 0)
        return true;

    BitstreamFilterContext bsf;
    if (!bsf.init("aac_adtstoasc", stream, error_text))
        return false;

    PacketHandle input_packet;
    PacketHandle filtered_packet;
    while (true) {
        int err = av_read_frame(input, input_packet.get());
        if (err == AVERROR_EOF) {
            error_text = "no AAC packet produced AudioSpecificConfig";
            return false;
        }
        if (err < 0) {
            error_text = "av_read_frame failed: " + ffmpeg_error_text(err);
            return false;
        }

        if (input_packet.get()->stream_index != stream->index) {
            input_packet.unref();
            continue;
        }

        err = av_bsf_send_packet(bsf.get(), input_packet.get());
        input_packet.unref();
        if (err < 0) {
            error_text = "av_bsf_send_packet failed: " + ffmpeg_error_text(err);
            return false;
        }

        while (true) {
            err = av_bsf_receive_packet(bsf.get(), filtered_packet.get());
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
                break;
            if (err < 0) {
                error_text = "av_bsf_receive_packet failed: " + ffmpeg_error_text(err);
                return false;
            }
            std::size_t side_data_size = 0;
            const std::uint8_t* side_data = av_packet_get_side_data(filtered_packet.get(), AV_PKT_DATA_NEW_EXTRADATA, &side_data_size);
            if (side_data != nullptr && side_data_size > 0) {
                return copy_codec_extradata(stream->codecpar, side_data, static_cast<int>(side_data_size), error_text);
            }
            filtered_packet.unref();
        }

        if (bsf.get()->par_out != nullptr && bsf.get()->par_out->extradata_size > 0)
            return copy_codec_extradata(stream->codecpar, bsf.get()->par_out->extradata, bsf.get()->par_out->extradata_size, error_text);
    }
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
                LOG << "Skipping unsupported video codec " << avcodec_get_name(stream->codecpar->codec_id)
                    << " in " << media_path;
            }
            continue;
        }

        if (!media.audio.present && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!fill_track_description(stream, false, media.audio)) {
                LOG << "Skipping unsupported audio codec " << avcodec_get_name(stream->codecpar->codec_id)
                    << " in " << media_path;
            }
        }
    }

    if (!media.video.present && !media.audio.present) {
        LOG << "No supported media tracks found in " << media_path;
        return false;
    }

    if (media.audio.present && !derive_aac_audio_specific_config(input.get(), input.get()->streams[media.audio.stream_index], error_text)) {
        LOG << "Failed to derive AAC AudioSpecificConfig for " << media_path << ": " << error_text;
        return false;
    }
    if (!rewind_input(input.get(), error_text)) {
        LOG << "Failed to rewind input after media inspection for " << media_path << ": " << error_text;
        return false;
    }

    if (!build_sdp(input.get(), media, media.sdp, error_text)) {
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

    if (impl_->audio_target.enabled && !derive_aac_audio_specific_config(impl_->input.get(), impl_->input.get()->streams[impl_->media.audio.stream_index], error_text)) {
        complete_startup(false, std::move(error_text));
        finalize();
        return;
    }
    if (!rewind_input(impl_->input.get(), error_text)) {
        complete_startup(false, std::move(error_text));
        finalize();
        return;
    }

    if (impl_->video_target.enabled) {
        impl_->video.input_index = impl_->media.video.stream_index;
        if (!impl_->video.output.init(make_rtp_url(impl_->video_target), impl_->input.get()->streams[impl_->video.input_index], impl_->rtp_cname, true, impl_->media.video.rtp_payload_type, error_text)) {
            complete_startup(false, std::move(error_text));
            finalize();
            return;
        }
    }

    if (impl_->audio_target.enabled) {
        impl_->audio.input_index = impl_->media.audio.stream_index;
        if (!impl_->audio.output.init(make_rtp_url(impl_->audio_target), impl_->input.get()->streams[impl_->audio.input_index], impl_->rtp_cname, true, impl_->media.audio.rtp_payload_type, error_text)) {
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
            impl_->packet.unref();
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
        impl_->packet.unref();
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
        impl_->packet.unref();
        if (err < 0) {
            LOG << impl_->log_prefix << " av_interleaved_write_frame failed: " << ffmpeg_error_text(err);
            finalize();
            return;
        }
    } else {
        impl_->packet.unref();
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
    impl_->packet.unref();

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
