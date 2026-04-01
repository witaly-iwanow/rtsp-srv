#include "session.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

struct RtspRequest {
    std::string method;
    std::string uri;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

int video_track_id(const MediaDescription& media) {
    return media.video.present ? 0 : -1;
}

int audio_track_id(const MediaDescription& media) {
    if (!media.audio.present)
        return -1;
    return media.video.present ? 1 : 0;
}

bool track_id_is_valid(const MediaDescription& media, int track_id) {
    return track_id == video_track_id(media) || track_id == audio_track_id(media);
}

bool track_id_is_video(const MediaDescription& media, int track_id) {
    return track_id == video_track_id(media);
}

bool track_id_is_audio(const MediaDescription& media, int track_id) {
    return track_id == audio_track_id(media);
}

std::string make_rtp_cname(std::uint32_t session_id) {
    return "rtsp-srv-" + std::to_string(session_id);
}

std::pair<std::uint16_t, std::uint16_t> make_server_ports(std::uint32_t session_id, std::uint16_t base_port) {
    constexpr std::uint16_t kPortSpan = 15000;
    constexpr std::uint16_t kPortsPerSession = 4;
    const std::uint16_t offset = static_cast<std::uint16_t>(((session_id - 1) * kPortsPerSession) % kPortSpan);
    const std::uint16_t rtp = static_cast<std::uint16_t>(base_port + offset);
    return {rtp, static_cast<std::uint16_t>(rtp + 1)};
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return value;
}

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return std::toupper(c);
    });
    return value;
}

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    const std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

std::string strip_track_suffix(const std::string& uri) {
    const std::size_t pos = uri.rfind("/trackID=");
    if (pos == std::string::npos)
        return uri;
    return uri.substr(0, pos);
}

bool parse_track_id(const std::string& uri, int& track_id) {
    const std::size_t pos = uri.rfind("/trackID=");
    if (pos == std::string::npos)
        return false;

    std::string value = uri.substr(pos + std::strlen("/trackID="));
    const std::size_t suffix = value.find_first_of("?#");
    if (suffix != std::string::npos)
        value = value.substr(0, suffix);
    if (value.empty())
        return false;

    int parsed = 0;
    try {
        parsed = std::stoi(value);
    } catch (const std::exception&) {
        return false;
    }

    if (std::to_string(parsed) != value)
        return false;

    track_id = parsed;
    return true;
}

std::size_t parse_content_length(const std::string& headers) {
    constexpr const char* kContentLength = "content-length:";
    std::size_t start = 0;
    while (start < headers.size()) {
        const std::size_t end = headers.find("\r\n", start);
        const std::size_t line_len = (end == std::string::npos ? headers.size() : end) - start;
        const std::string line = headers.substr(start, line_len);
        const std::string lower_line = to_lower(line);
        if (lower_line.rfind(kContentLength, 0) == 0) {
            const std::string raw = trim(line.substr(std::strlen(kContentLength)));
            if (raw.empty())
                return 0;
            try {
                return static_cast<std::size_t>(std::stoul(raw));
            } catch (const std::exception&) {
                return 0;
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 2;
    }
    return 0;
}

bool extract_next_rtsp_request(std::string& pending, std::string& request_out) {
    const std::size_t headers_end = pending.find("\r\n\r\n");
    if (headers_end == std::string::npos)
        return false;

    const std::size_t headers_size = headers_end + 4;
    const std::size_t content_length = parse_content_length(pending.substr(0, headers_end));
    const std::size_t total_size = headers_size + content_length;
    if (pending.size() < total_size)
        return false;

    request_out = pending.substr(0, total_size);
    pending.erase(0, total_size);
    return true;
}

bool parse_rtsp_request(const std::string& raw, RtspRequest& request) {
    const std::size_t headers_end = raw.find("\r\n\r\n");
    if (headers_end == std::string::npos)
        return false;
    const std::size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos)
        return false;

    const std::string first_line = raw.substr(0, line_end);
    std::istringstream iss(first_line);
    if (!(iss >> request.method >> request.uri >> request.version))
        return false;

    std::size_t cursor = line_end + 2;
    while (cursor < headers_end) {
        const std::size_t next = raw.find("\r\n", cursor);
        if (next == std::string::npos || next > headers_end)
            return false;

        const std::string line = raw.substr(cursor, next - cursor);
        const std::size_t sep = line.find(':');
        if (sep != std::string::npos) {
            const std::string key = to_lower(trim(line.substr(0, sep)));
            const std::string value = trim(line.substr(sep + 1));
            if (!key.empty())
                request.headers[key] = value;
        }
        cursor = next + 2;
    }

    request.body = raw.substr(headers_end + 4);
    request.method = to_upper(request.method);
    return true;
}

std::string get_header(const RtspRequest& request, const std::string& key_lower) {
    const auto it = request.headers.find(key_lower);
    if (it == request.headers.end())
        return "";
    return it->second;
}

bool parse_client_ports(const std::string& transport, std::uint16_t& rtp, std::uint16_t& rtcp) {
    const std::string lower = to_lower(transport);
    const std::size_t pos = lower.find("client_port=");
    if (pos == std::string::npos)
        return false;

    const std::size_t value_start = pos + std::strlen("client_port=");
    std::size_t dash = lower.find('-', value_start);
    if (dash == std::string::npos)
        return false;
    std::size_t end = dash + 1;
    while (end < lower.size() && std::isdigit(static_cast<unsigned char>(lower[end])))
        ++end;

    const std::string rtp_text = lower.substr(value_start, dash - value_start);
    const std::string rtcp_text = lower.substr(dash + 1, end - (dash + 1));
    if (rtp_text.empty() || rtcp_text.empty())
        return false;

    try {
        const int parsed_rtp = std::stoi(rtp_text);
        const int parsed_rtcp = std::stoi(rtcp_text);
        if (parsed_rtp <= 0 || parsed_rtp > 65535 || parsed_rtcp <= 0 || parsed_rtcp > 65535)
            return false;
        rtp = static_cast<std::uint16_t>(parsed_rtp);
        rtcp = static_cast<std::uint16_t>(parsed_rtcp);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_media_relative_path(const std::string& uri, std::filesystem::path& relative_path) {
    std::string path = uri;
    if (path.rfind("rtsp://", 0) == 0) {
        const std::size_t host_start = std::strlen("rtsp://");
        const std::size_t slash = path.find('/', host_start);
        if (slash == std::string::npos)
            return false;
        path = path.substr(slash + 1);
    } else if (!path.empty() && path.front() == '/') {
        path = path.substr(1);
    }

    const std::size_t query = path.find_first_of("?#");
    if (query != std::string::npos)
        path = path.substr(0, query);
    if (path.empty())
        return false;

    std::filesystem::path candidate(path);
    if (candidate.is_absolute())
        return false;
    for (const std::filesystem::path& part: candidate)
        if (part == "..")
            return false;
    relative_path = candidate.lexically_normal();
    return !relative_path.empty();
}

std::string cseq_or_zero(const std::string& cseq) {
    if (cseq.empty())
        return "0";
    return cseq;
}

std::string session_id_only(const std::string& session_header) {
    const std::size_t semi = session_header.find(';');
    if (semi == std::string::npos)
        return trim(session_header);
    return trim(session_header.substr(0, semi));
}

std::string endpoint_host(const std::string& endpoint) {
    if (endpoint.empty())
        return "";
    if (endpoint.front() == '[') {
        const std::size_t close = endpoint.find(']');
        if (close == std::string::npos || close <= 1)
            return "";
        return endpoint.substr(1, close - 1);
    }
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0)
        return "";
    return endpoint.substr(0, colon);
}

}  // namespace

Session::Session(
    Socket socket,
    std::string remote_endpoint,
    const std::filesystem::path& media_dir,
    std::uint32_t session_id,
    CloseHandler on_close):
    socket_(std::move(socket)),
    remote_endpoint_(std::move(remote_endpoint)),
    media_dir_(media_dir),
    session_id_(session_id),
    on_close_(std::move(on_close)) {
    const auto [video_rtp, video_rtcp] = make_server_ports(session_id_, 50000);
    video_ports_.server_rtp = video_rtp;
    video_ports_.server_rtcp = video_rtcp;

    const auto [audio_rtp, audio_rtcp] = make_server_ports(session_id_, 50002);
    audio_ports_.server_rtp = audio_rtp;
    audio_ports_.server_rtcp = audio_rtcp;
}

Session::~Session() {
    stop_streaming();
}

void Session::start() {
    start_read();
}

void Session::shutdown() {
    if (finished_)
        return;

    stop_streaming();
    queue_response(make_response(
        503,
        "Service Unavailable",
        "0",
        {{"Connection", "close"}, {"Reason", "server shutting down"}},
        "",
        true));
}

const std::string& Session::remote_endpoint() const {
    return remote_endpoint_;
}

std::uint32_t Session::session_id() const {
    return session_id_;
}

std::string Session::log_prefix() const {
    std::ostringstream prefix;
    prefix << '[' << std::hex << std::nouppercase << std::setw(6) << std::setfill('0') << (session_id_ & 0xFFFFFF) << ']';
    return prefix.str();
}

Logger::Entry Session::log() const {
    return Logger::Entry(Logger::instance(), log_prefix() + " ");
}

std::string Session::session_id_text() const {
    return std::to_string(session_id_);
}

bool Session::has_setup_tracks() const {
    return video_ports_.setup || audio_ports_.setup;
}

void Session::reset_track_state() {
    video_ports_.setup = false;
    audio_ports_.setup = false;
    current_media_ = {};
}

Session::RequestOutcome Session::make_response(
    int status_code,
    const std::string& reason,
    const std::string& cseq,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    bool close_after_response) const {
    std::ostringstream response;
    response << "RTSP/1.0 " << status_code << ' ' << reason << "\r\n";
    response << "CSeq: " << cseq_or_zero(cseq) << "\r\n";
    for (const auto& [key, value]: headers)
        response << key << ": " << value << "\r\n";
    if (!body.empty())
        response << "Content-Length: " << body.size() << "\r\n";
    response << "\r\n";
    response << body;
    return {response.str(), close_after_response};
}

void Session::start_read() {
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(read_buffer_), [self](asio::error_code ec, std::size_t bytes_read) {
        self->handle_read(ec, bytes_read);
    });
}

void Session::handle_read(asio::error_code ec, std::size_t bytes_read) {
    if (finished_)
        return;
    if (ec) {
        if (ec != asio::error::eof && ec != asio::error::operation_aborted)
            log() << "read failed from " << remote_endpoint_ << ": " << ec.message();
        finish();
        return;
    }

    pending_requests_.append(read_buffer_.data(), bytes_read);
    process_pending_requests();
    if (!finished_)
        start_read();
}

void Session::process_pending_requests() {
    while (!finished_) {
        std::string request;
        if (!extract_next_rtsp_request(pending_requests_, request))
            return;

        RequestOutcome outcome = handle_request(request);
        const bool close_after_response = outcome.close_after_response;
        queue_response(std::move(outcome));
        if (close_after_response)
            return;
    }
}

void Session::queue_response(RequestOutcome outcome) {
    if (finished_)
        return;

    close_after_write_ = close_after_write_ || outcome.close_after_response;
    const bool write_idle = write_queue_.empty();
    write_queue_.push_back(std::move(outcome.response));
    if (write_idle)
        start_write();
}

void Session::start_write() {
    if (finished_ || write_queue_.empty())
        return;

    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(write_queue_.front()), [self](asio::error_code ec, std::size_t bytes_written) {
        self->handle_write(ec, bytes_written);
    });
}

void Session::handle_write(asio::error_code ec, std::size_t) {
    if (finished_)
        return;
    if (ec) {
        if (ec != asio::error::operation_aborted)
            log() << "write failed to " << remote_endpoint_ << ": " << ec.message();
        finish();
        return;
    }

    if (!write_queue_.empty())
        write_queue_.pop_front();
    if (!write_queue_.empty()) {
        start_write();
        return;
    }

    if (close_after_write_)
        finish();
}

void Session::close_socket() {
    if (!socket_.is_open())
        return;

    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

void Session::finish() {
    if (finished_)
        return;

    finished_ = true;
    close_after_write_ = true;
    pending_requests_.clear();
    write_queue_.clear();
    close_socket();
    stop_streaming();

    CloseHandler on_close = std::move(on_close_);
    if (on_close)
        on_close(session_id_, remote_endpoint_);
}

bool Session::load_media_description(const std::filesystem::path& media_path, const std::string& media_uri) {
    if (current_media_path_ == media_path && current_media_uri_ == media_uri && !current_media_.sdp.empty())
        return true;

    MediaDescription media;
    if (!describe_media(media_path, media))
        return false;

    current_media_path_ = media_path;
    current_media_uri_ = media_uri;
    current_media_ = std::move(media);

    if (current_media_.video.present) {
        log() << "selected video codec " << current_media_.video.codec_name
              << (current_media_.video.copy ? " via passthrough" : " via libx264 transcode");
    } else {
        log() << "no video track detected";
    }
    if (current_media_.audio.present) {
        log() << "selected audio codec " << current_media_.audio.codec_name
              << (current_media_.audio.copy ? " via passthrough" : " via libopus transcode");
    } else {
        log() << "no audio track detected";
    }

    return true;
}

bool Session::start_streaming() {
    if (streamer_ && streamer_->running())
        return true;
    if (current_media_path_.empty() || current_media_.sdp.empty())
        return false;

    const std::string host = endpoint_host(remote_endpoint_);
    if (host.empty())
        return false;

    const bool stream_video = current_media_.video.present && video_ports_.setup && video_ports_.client_rtp != 0;
    const bool stream_audio = current_media_.audio.present && audio_ports_.setup && audio_ports_.client_rtp != 0;
    if (!stream_video && !stream_audio)
        return false;

    StreamTarget video_target;
    if (stream_video) {
        video_target.enabled = true;
        video_target.host = host;
        video_target.client_rtp = video_ports_.client_rtp;
        video_target.client_rtcp = video_ports_.client_rtcp;
        video_target.server_rtp = video_ports_.server_rtp;
        video_target.server_rtcp = video_ports_.server_rtcp;
    }

    StreamTarget audio_target;
    if (stream_audio) {
        audio_target.enabled = true;
        audio_target.host = host;
        audio_target.client_rtp = audio_ports_.client_rtp;
        audio_target.client_rtcp = audio_ports_.client_rtcp;
        audio_target.server_rtp = audio_ports_.server_rtp;
        audio_target.server_rtcp = audio_ports_.server_rtcp;
    }

    auto streamer = std::make_unique<MediaStreamer>(
        current_media_path_,
        current_media_,
        video_target,
        audio_target,
        make_rtp_cname(session_id_),
        log_prefix());
    if (!streamer->start())
        return false;

    streamer_ = std::move(streamer);
    return true;
}

void Session::stop_streaming() {
    if (!streamer_)
        return;
    streamer_->stop();
    streamer_.reset();
}

Session::RequestOutcome Session::handle_options(const std::string& cseq) const {
    return make_response(200, "OK", cseq, {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN"}}, "");
}

Session::RequestOutcome Session::handle_describe(const std::string& uri, const std::string& cseq) {
    std::filesystem::path rel_path;
    if (!parse_media_relative_path(uri, rel_path))
        return make_response(400, "Bad Request", cseq, {}, "");

    const auto media_path = media_dir_ / rel_path;
    if (!std::filesystem::exists(media_path))
        return make_response(404, "Not Found", cseq, {}, "");

    const std::string media_uri = strip_track_suffix(uri);
    if (!load_media_description(media_path, media_uri))
        return make_response(415, "Unsupported Media Type", cseq, {}, "");

    std::string base = current_media_uri_;
    if (!base.empty() && base.back() != '/')
        base += '/';
    return make_response(
        200,
        "OK",
        cseq,
        {{"Content-Base", base}, {"Content-Type", "application/sdp"}},
        current_media_.sdp);
}

Session::RequestOutcome Session::handle_setup(
    const std::string& uri,
    const std::string& cseq,
    const std::string& transport,
    const std::string& session_header) {
    if (transport.empty())
        return make_response(461, "Unsupported Transport", cseq, {}, "");

    std::uint16_t parsed_rtp = 0;
    std::uint16_t parsed_rtcp = 0;
    if (!parse_client_ports(transport, parsed_rtp, parsed_rtcp))
        return make_response(461, "Unsupported Transport", cseq, {}, "");

    const std::string media_uri = strip_track_suffix(uri);
    std::filesystem::path rel_path;
    if (!parse_media_relative_path(media_uri, rel_path))
        return make_response(400, "Bad Request", cseq, {}, "");

    const auto media_path = media_dir_ / rel_path;
    if (!std::filesystem::exists(media_path))
        return make_response(404, "Not Found", cseq, {}, "");

    int track_id = -1;
    if (!parse_track_id(uri, track_id))
        return make_response(400, "Bad Request", cseq, {}, "");

    if (has_setup_tracks()) {
        const std::string req_session = session_id_only(session_header);
        if (!req_session.empty() && req_session != session_id_text())
            return make_response(454, "Session Not Found", cseq, {}, "");
    }

    if (!current_media_path_.empty() && current_media_path_ != media_path) {
        stop_streaming();
        reset_track_state();
    }

    if (!load_media_description(media_path, media_uri))
        return make_response(415, "Unsupported Media Type", cseq, {}, "");

    if (!track_id_is_valid(current_media_, track_id))
        return make_response(404, "Not Found", cseq, {}, "");

    TrackPorts* ports = nullptr;
    if (track_id_is_video(current_media_, track_id))
        ports = &video_ports_;
    else if (track_id_is_audio(current_media_, track_id))
        ports = &audio_ports_;
    else
        return make_response(404, "Not Found", cseq, {}, "");

    ports->client_rtp = parsed_rtp;
    ports->client_rtcp = parsed_rtcp;
    ports->setup = true;

    std::ostringstream transport_reply;
    transport_reply << "RTP/AVP;unicast;client_port=" << ports->client_rtp << '-' << ports->client_rtcp
                    << ";server_port=" << ports->server_rtp << '-' << ports->server_rtcp;
    return make_response(
        200,
        "OK",
        cseq,
        {{"Session", session_id_text()}, {"Transport", transport_reply.str()}},
        "");
}

Session::RequestOutcome Session::handle_play(const std::string& cseq, const std::string& session_header) {
    if (!has_setup_tracks())
        return make_response(454, "Session Not Found", cseq, {}, "");

    const std::string req_session = session_id_only(session_header);
    if (!req_session.empty() && req_session != session_id_text())
        return make_response(454, "Session Not Found", cseq, {}, "");

    const bool can_play_video = current_media_.video.present && video_ports_.setup && video_ports_.client_rtp != 0;
    const bool can_play_audio = current_media_.audio.present && audio_ports_.setup && audio_ports_.client_rtp != 0;
    if (current_media_path_.empty() || (!can_play_video && !can_play_audio))
        return make_response(455, "Method Not Valid In This State", cseq, {}, "");
    if (!std::filesystem::exists(current_media_path_))
        return make_response(404, "Not Found", cseq, {}, "");

    if (!start_streaming())
        return make_response(500, "Internal Server Error", cseq, {}, "");

    return make_response(200, "OK", cseq, {{"Session", session_id_text()}}, "");
}

Session::RequestOutcome Session::handle_teardown(const std::string& cseq) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (has_setup_tracks())
        headers.emplace_back("Session", session_id_text());
    stop_streaming();
    return make_response(200, "OK", cseq, headers, "", true);
}

Session::RequestOutcome Session::handle_request(const std::string& raw_request) {
    RtspRequest request;
    if (!parse_rtsp_request(raw_request, request))
        return make_response(400, "Bad Request", "0", {}, "");

    log() << request.method;
    const std::string cseq = get_header(request, "cseq");

    if (request.method == "OPTIONS")
        return handle_options(cseq);
    if (request.method == "DESCRIBE")
        return handle_describe(request.uri, cseq);
    if (request.method == "SETUP")
        return handle_setup(request.uri, cseq, get_header(request, "transport"), get_header(request, "session"));
    if (request.method == "PLAY")
        return handle_play(cseq, get_header(request, "session"));
    if (request.method == "TEARDOWN")
        return handle_teardown(cseq);

    return make_response(501, "Not Implemented", cseq, {}, "");
}
