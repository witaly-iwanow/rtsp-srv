#include "session.h"
#include "logger.h"
#include "utils.h"

#include <charconv>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
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

std::string strip_track_suffix(const std::string& uri) {
    const std::size_t pos = uri.rfind("/trackID=");
    if (pos == std::string::npos)
        return uri;
    return uri.substr(0, pos);
}

bool has_track_suffix(const std::string& uri) {
    return uri.rfind("/trackID=") != std::string::npos;
}

bool parse_track_id(const std::string& uri, int& track_id) {
    const std::size_t pos = uri.rfind("/trackID=");
    if (pos == std::string::npos)
        return false;

    std::string_view value(uri.data() + pos + std::strlen("/trackID="), uri.size() - pos - std::strlen("/trackID="));
    const std::size_t suffix = value.find_first_of("?#");
    if (suffix != std::string::npos)
        value = value.substr(0, suffix);
    if (value.empty())
        return false;

    int parsed = -1;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc() || ptr != value.data() + value.size())
        return false;
    if (parsed < 0)
        return false;

    track_id = parsed;
    return true;
}

std::size_t parse_content_length(const std::string& headers) {
    constexpr std::string_view kContentLength = "content-length:";
    const std::string lower_headers = util::to_lower(headers);

    std::size_t pos = 0;
    while ((pos = lower_headers.find(kContentLength, pos)) != std::string::npos) {
        if (pos != 0 && lower_headers.compare(pos - 2, 2, "\r\n") != 0) {
            pos += kContentLength.size();
            continue;
        }

        const std::size_t value_start = pos + kContentLength.size();
        const std::size_t value_end = headers.find("\r\n", value_start);
        const std::string_view raw(headers.data() + value_start, (value_end == std::string::npos ? headers.size() : value_end) - value_start);
        const std::string trimmed = util::trim(raw);
        if (trimmed.empty())
            return 0;

        std::size_t content_length = 0;
        const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), content_length);
        if (ec == std::errc() && ptr == trimmed.data() + trimmed.size())
            return content_length;
        return 0;
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
            const std::string key = util::to_lower(util::trim(line.substr(0, sep)));
            const std::string value = util::trim(line.substr(sep + 1));
            if (!key.empty())
                request.headers[key] = value;
        }
        cursor = next + 2;
    }

    request.body = raw.substr(headers_end + 4);
    request.method = util::to_upper(request.method);
    return true;
}

std::string get_header(const RtspRequest& request, const std::string& key_lower) {
    const auto it = request.headers.find(key_lower);
    if (it == request.headers.end())
        return "";
    return it->second;
}

bool parse_client_ports(const std::string& transport, std::uint16_t& rtp, std::uint16_t& rtcp) {
    const std::string lower = util::to_lower(transport);
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

bool port_is_reserved(std::uint16_t port, const std::vector<std::uint16_t>& reserved_ports) {
    for (const std::uint16_t reserved: reserved_ports)
        if (reserved != 0 && reserved == port)
            return true;
    return false;
}

bool allocate_server_ports(
    Session::Socket& socket,
    const std::vector<std::uint16_t>& reserved_ports,
    std::uint16_t& server_rtp,
    std::uint16_t& server_rtcp) {
    using asio::ip::udp;

    asio::error_code ec;
    const auto local_endpoint = socket.local_endpoint(ec);
    if (ec)
        return false;

    const udp::endpoint bind_endpoint(local_endpoint.address(), 0);
    for (int attempt = 0; attempt < 64; ++attempt) {
        // Pick an OS-assigned even RTP port, then reserve RTP+1 for RTCP.
        udp::socket rtp_socket(socket.get_executor());
        rtp_socket.open(bind_endpoint.protocol(), ec);
        if (ec)
            continue;

        rtp_socket.bind(bind_endpoint, ec);
        if (ec)
            continue;

        const auto rtp_endpoint = rtp_socket.local_endpoint(ec);
        if (ec)
            continue;

        const std::uint16_t rtp_port = rtp_endpoint.port();
        if ((rtp_port & 1u) != 0 || rtp_port == std::numeric_limits<std::uint16_t>::max())
            continue;
        if (port_is_reserved(rtp_port, reserved_ports) || port_is_reserved(static_cast<std::uint16_t>(rtp_port + 1), reserved_ports))
            continue;

        udp::socket rtcp_socket(socket.get_executor());
        rtcp_socket.open(bind_endpoint.protocol(), ec);
        if (ec)
            continue;

        rtcp_socket.bind(udp::endpoint(bind_endpoint.address(), static_cast<std::uint16_t>(rtp_port + 1)), ec);
        if (ec)
            continue;

        server_rtp = rtp_port;
        server_rtcp = static_cast<std::uint16_t>(rtp_port + 1);
        return true;
    }

    return false;
}

// We only support plain UDP unicast RTP/RTCP via RTP/AVP + client_port=...
// Multicast and interleaved RTP-over-RTSP/TCP transports are rejected
bool is_supported_transport(const std::string& transport) {
    const std::string lower = util::to_lower(transport);
    const std::size_t first_sep = lower.find(';');
    const std::string profile = util::trim(lower.substr(0, first_sep));
    if (profile != "rtp/avp" && profile != "rtp/avp/udp")
        return false;

    std::size_t start = first_sep == std::string::npos ? lower.size() : first_sep + 1;
    while (start < lower.size()) {
        const std::size_t end = lower.find(';', start);
        const std::string token = util::trim(lower.substr(start, end - start));
        if (token == "multicast" || token.rfind("interleaved=", 0) == 0)
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }

    return true;
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
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
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

std::string session_id_only(const std::string& session_header) {
    const std::size_t semi = session_header.find(';');
    if (semi == std::string::npos)
        return util::trim(session_header);
    return util::trim(session_header.substr(0, semi));
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

Session::Session(Socket socket, std::string remote_endpoint, const std::filesystem::path& media_dir, MediaExecutor media_executor, std::uint32_t session_id, CloseHandler on_close):
    socket_(std::move(socket)),
    remote_endpoint_(std::move(remote_endpoint)),
    media_dir_(media_dir),
    media_executor_(std::move(media_executor)),
    session_id_(session_id),
    on_close_(std::move(on_close)) {}

Session::~Session() {
    streamer_.reset();
}

void Session::start() {
    start_read();
}

void Session::shutdown() {
    if (finished_)
        return;

    RequestOutcome outcome = make_response(503, "Service Unavailable", "0", {{"Connection", "close"}, {"Reason", "server shutting down"}}, "", true);
    if (stream_state_ == StreamState::Idle && !streamer_) {
        queue_response(std::move(outcome));
        return;
    }

    stop_playback(std::move(outcome), false);
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
    response << "CSeq: " << cseq << "\r\n";
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
    // PLAY/TEARDOWN responses are completed later from media callbacks.
    if (stream_state_ == StreamState::Starting || stream_state_ == StreamState::Stopping)
        return;

    while (!finished_) {
        std::string request;
        if (!extract_next_rtsp_request(pending_requests_, request))
            return;

        std::optional<RequestOutcome> outcome = handle_request(request);
        if (!outcome)
            return;

        const bool close_after_response = outcome->close_after_response;
        queue_response(std::move(*outcome));
        if (close_after_response)
            return;

        if (stream_state_ == StreamState::Starting || stream_state_ == StreamState::Stopping)
            return;
    }
}

void Session::start_playback(std::string cseq) {
    if (!streamer_) {
        queue_response(make_response(500, "Internal Server Error", cseq, {}, ""));
        return;
    }

    stream_state_ = StreamState::Starting;
    const auto executor = socket_.get_executor();
    std::weak_ptr<Session> weak_self(shared_from_this());
    // Complete startup on the media strand, but resume RTSP handling on the socket executor.
    streamer_->start([weak_self, executor, cseq = std::move(cseq)](bool ok, std::string error_text) mutable {
        asio::post(executor, [weak_self, ok, cseq = std::move(cseq), error_text = std::move(error_text)]() mutable {
            if (auto self = weak_self.lock())
                self->handle_playback_started(ok, std::move(cseq), std::move(error_text));
        });
    });
}

void Session::handle_playback_started(bool ok, std::string cseq, std::string) {
    if (finished_)
        return;

    if (stream_state_ == StreamState::Stopping) {
        if (!ok)
            streamer_.reset();
        return;
    }

    if (stream_state_ != StreamState::Starting)
        return;

    if (!ok) {
        stream_state_ = StreamState::Idle;
        streamer_.reset();
        queue_response(make_response(500, "Internal Server Error", cseq, {}, ""));
        process_pending_requests();
        return;
    }

    stream_state_ = StreamState::Playing;
    queue_response(make_response(200, "OK", cseq, {{"Session", session_id_text()}}, ""));
    process_pending_requests();
}

void Session::stop_playback(std::optional<RequestOutcome> response, bool finish_after_stop) {
    if (response)
        pending_stop_response_ = std::move(response);
    finish_after_stop_ = finish_after_stop_ || finish_after_stop;

    if (!streamer_) {
        stream_state_ = StreamState::Idle;
        if (finish_after_stop_) {
            finish_after_stop_ = false;
            finalize_close();
            return;
        }
        if (pending_stop_response_) {
            RequestOutcome outcome = std::move(*pending_stop_response_);
            pending_stop_response_.reset();
            const bool close_after_response = outcome.close_after_response;
            queue_response(std::move(outcome));
            if (close_after_response)
                return;
        }
        process_pending_requests();
        return;
    }

    if (stream_state_ == StreamState::Stopping)
        return;

    // Multiple stop triggers can race; only the first one actually drives the streamer.
    stream_state_ = StreamState::Stopping;
    const auto executor = socket_.get_executor();
    std::weak_ptr<Session> weak_self(shared_from_this());
    streamer_->stop([weak_self, executor]() {
        asio::post(executor, [weak_self]() {
            if (auto self = weak_self.lock())
                self->handle_playback_stopped();
        });
    });
}

void Session::handle_playback_stopped() {
    stream_state_ = StreamState::Idle;
    streamer_.reset();

    if (finished_ || finish_after_stop_) {
        finish_after_stop_ = false;
        finalize_close();
        return;
    }

    if (pending_stop_response_) {
        RequestOutcome outcome = std::move(*pending_stop_response_);
        pending_stop_response_.reset();
        const bool close_after_response = outcome.close_after_response;
        queue_response(std::move(outcome));
        if (close_after_response)
            return;
    }

    process_pending_requests();
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

void Session::finalize_close() {
    try {
        CloseHandler on_close = std::move(on_close_);
        if (on_close)
            on_close(session_id_, remote_endpoint_);
    } catch (...) {
        log() << "on_close callback threw an exception";
    }
}

void Session::finish() {
    if (finished_)
        return;

    finished_ = true;
    close_after_write_ = true;
    pending_requests_.clear();
    write_queue_.clear();
    pending_stop_response_.reset();
    close_socket();
    if (streamer_ || stream_state_ == StreamState::Starting || stream_state_ == StreamState::Playing || stream_state_ == StreamState::Stopping) {
        stop_playback(std::nullopt, true);
        return;
    }

    finalize_close();
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

    if (current_media_.video.present)
        log() << "selected video codec " << current_media_.video.codec_name;
    else
        log() << "no supported video track detected";

    if (current_media_.audio.present)
        log() << "selected audio codec " << current_media_.audio.codec_name;
    else
        log() << "no supported audio track detected";

    return true;
}

bool Session::create_streamer() {
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

    streamer_ = std::make_unique<MediaStreamer>(media_executor_, current_media_path_, current_media_, video_target, audio_target, make_rtp_cname(session_id_), log_prefix());
    return true;
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
        return make_response(404, "Not Found", cseq, {}, "");

    std::string base = current_media_uri_;
    if (!base.empty() && base.back() != '/')
        base += '/';
    return make_response(200, "OK", cseq, {{"Content-Base", base}, {"Content-Type", "application/sdp"}}, current_media_.sdp);
}

Session::RequestOutcome Session::handle_setup(const std::string& uri, const std::string& cseq, const std::string& transport, const std::string& session_header) {
    if (stream_state_ == StreamState::Starting || stream_state_ == StreamState::Stopping)
        return make_response(455, "Method Not Valid In This State", cseq, {}, "");

    if (transport.empty())
        return make_response(461, "Unsupported Transport", cseq, {}, "");
    if (!is_supported_transport(transport))
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

    if (has_setup_tracks()) {
        const std::string req_session = session_id_only(session_header);
        if (!req_session.empty() && req_session != session_id_text())
            return make_response(454, "Session Not Found", cseq, {}, "");
    }

    if (!current_media_path_.empty() && current_media_path_ != media_path) {
        if (stream_state_ != StreamState::Idle)
            return make_response(455, "Method Not Valid In This State", cseq, {}, "");
        streamer_.reset();
        reset_track_state();
    }

    if (!load_media_description(media_path, media_uri))
        return make_response(404, "Not Found", cseq, {}, "");

    // Accept aggregate SETUP only for single-track media; for A+V, require explicit /trackID=...
    int track_id = -1;
    if (!parse_track_id(uri, track_id)) {
        if (has_track_suffix(uri))
            return make_response(400, "Bad Request", cseq, {}, "");

        const bool has_video = current_media_.video.present;
        const bool has_audio = current_media_.audio.present;
        if (has_video == has_audio)
            return make_response(400, "Bad Request", cseq, {}, "");

        track_id = has_video ? video_track_id(current_media_) : audio_track_id(current_media_);
    }

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
    // Avoid reusing the client's ports when server and client run on the same host.
    std::vector<std::uint16_t> reserved_ports = {
        video_ports_.client_rtp,
        video_ports_.client_rtcp,
        audio_ports_.client_rtp,
        audio_ports_.client_rtcp,
        video_ports_.server_rtp,
        video_ports_.server_rtcp,
        audio_ports_.server_rtp,
        audio_ports_.server_rtcp,
    };
    if (!allocate_server_ports(socket_, reserved_ports, ports->server_rtp, ports->server_rtcp))
        return make_response(500, "Internal Server Error", cseq, {}, "");
    ports->setup = true;

    std::string transport_reply = "RTP/AVP;unicast;client_port=" + std::to_string(ports->client_rtp) + "-" + std::to_string(ports->client_rtcp);
    transport_reply += ";server_port=" + std::to_string(ports->server_rtp) + "-" + std::to_string(ports->server_rtcp);
    return make_response(200, "OK", cseq, {{"Session", session_id_text()}, {"Transport", transport_reply}}, "");
}

std::optional<Session::RequestOutcome> Session::handle_play(const std::string& cseq, const std::string& session_header) {
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

    if (stream_state_ == StreamState::Starting || stream_state_ == StreamState::Stopping)
        return make_response(455, "Method Not Valid In This State", cseq, {}, "");
    if (stream_state_ == StreamState::Playing)
        return make_response(200, "OK", cseq, {{"Session", session_id_text()}}, "");
    if (!create_streamer())
        return make_response(500, "Internal Server Error", cseq, {}, "");

    start_playback(cseq);
    return std::nullopt;
}

std::optional<Session::RequestOutcome> Session::handle_teardown(const std::string& cseq) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (has_setup_tracks())
        headers.emplace_back("Session", session_id_text());

    if (stream_state_ == StreamState::Idle && !streamer_)
        return make_response(200, "OK", cseq, headers, "", true);

    stop_playback(make_response(200, "OK", cseq, headers, "", true), false);
    return std::nullopt;
}

std::optional<Session::RequestOutcome> Session::handle_request(const std::string& raw_request) {
    RtspRequest request;
    if (!parse_rtsp_request(raw_request, request))
        return make_response(400, "Bad Request", "0", {}, "");

    log() << request.method;
    const std::string cseq = get_header(request, "cseq");
    if (cseq.empty())
        return make_response(400, "Bad Request", "0", {}, "");

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
