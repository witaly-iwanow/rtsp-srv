#include "session.h"
#include "logger.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace {

struct RtspRequest {
    std::string method;
    std::string uri;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

constexpr std::uint16_t kDescribeVideoRtpPort = 40000;
constexpr std::uint16_t kDescribeAudioRtpPort = 40002;

std::string make_rtp_url(const std::string& host, std::uint16_t port);

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

bool generate_sdp(const std::filesystem::path& media_path, const MediaDescription& media, std::string& sdp_out) {
    char temp_path[] = "/tmp/rtsp-sdp-XXXXXX";
    const int temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        LOG << "mkstemp() failed: " << std::strerror(errno);
        return false;
    }
    close(temp_fd);

    const std::string sdp_file = temp_path;
    const std::string video_rtp_url = make_rtp_url("127.0.0.1", kDescribeVideoRtpPort);
    const std::string audio_rtp_url = make_rtp_url("127.0.0.1", kDescribeAudioRtpPort);
    const std::string* video_url = media.video.present ? &video_rtp_url : nullptr;
    const std::string* audio_url = media.audio.present ? &audio_rtp_url : nullptr;
    const std::string rtp_cname = "rtsp-sdp";
    const std::vector<std::string> args =
        make_ffmpeg_args(media_path, media, video_url, audio_url, &rtp_cname, false, false, &sdp_file);

    LOG << "Generating SDP via ffmpeg: " << join_command(args);
    const pid_t pid = spawn_process(args);
    if (pid < 0) {
        LOG << "fork() failed for ffmpeg SDP generation: " << std::strerror(errno);
        unlink(temp_path);
        return false;
    }

    std::string raw_sdp;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!std::filesystem::exists(sdp_file))
            continue;
        if (std::filesystem::file_size(sdp_file) == 0)
            continue;

        std::ifstream input(sdp_file);
        raw_sdp.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (!raw_sdp.empty())
            break;
    }

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        LOG << "kill(SIGTERM) failed for ffmpeg SDP pid " << pid << ": " << std::strerror(errno);
    wait_for_process(pid, "ffmpeg", false);
    unlink(temp_path);

    if (raw_sdp.empty())
        return false;

    sdp_out = normalize_sdp(raw_sdp, media_path.filename().string());
    return true;
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

std::string make_rtp_url(const std::string& host, std::uint16_t port) {
    if (host.find(':') != std::string::npos)
        return "rtp://[" + host + "]:" + std::to_string(port) + "?pkt_size=1200";
    return "rtp://" + host + ":" + std::to_string(port) + "?pkt_size=1200";
}

}  // namespace

Session::Session(
    int client_fd,
    const std::string& remote_endpoint,
    const std::filesystem::path& media_dir,
    std::uint32_t session_id):
    client_fd_(client_fd), remote_endpoint_(remote_endpoint), media_dir_(media_dir), session_id_(session_id) {}

Session::~Session() {
    stop_streaming();

    const std::lock_guard<std::mutex> lock(fd_mutex_);
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }
}

void Session::shutdown() {
    stop_streaming();

    const std::lock_guard<std::mutex> lock(fd_mutex_);
    if (client_fd_ < 0)
        return;

    constexpr char kShutdownMsg[] =
        "RTSP/1.0 503 Service Unavailable\r\n"
        "CSeq: 0\r\n"
        "Connection: close\r\n"
        "Reason: server shutting down\r\n"
        "\r\n";

    int send_flags = 0;
#ifdef MSG_NOSIGNAL
    send_flags = MSG_NOSIGNAL;
#endif
    static_cast<void>(send(client_fd_, kShutdownMsg, sizeof(kShutdownMsg) - 1, send_flags));
    ::shutdown(client_fd_, SHUT_RDWR);
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

bool Session::send_raw(const std::string& data) {
    const std::lock_guard<std::mutex> lock(fd_mutex_);
    if (client_fd_ < 0)
        return false;

    std::size_t sent = 0;
    int send_flags = 0;
#ifdef MSG_NOSIGNAL
    send_flags = MSG_NOSIGNAL;
#endif
    while (sent < data.size()) {
        const ssize_t n = send(client_fd_, data.data() + sent, data.size() - sent, send_flags);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool Session::send_response(
    int status_code,
    const std::string& reason,
    const std::string& cseq,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body) {
    std::ostringstream response;
    response << "RTSP/1.0 " << status_code << ' ' << reason << "\r\n";
    response << "CSeq: " << cseq_or_zero(cseq) << "\r\n";
    for (const auto& [key, value]: headers)
        response << key << ": " << value << "\r\n";
    if (!body.empty())
        response << "Content-Length: " << body.size() << "\r\n";
    response << "\r\n";
    response << body;
    return send_raw(response.str());
}

bool Session::load_media_description(const std::filesystem::path& media_path, const std::string& media_uri) {
    if (current_media_path_ == media_path && current_media_uri_ == media_uri && !current_media_.sdp.empty())
        return true;

    MediaDescription media;
    if (!probe_track(media_path, "v:0", true, media.video))
        return false;
    if (!probe_track(media_path, "a:0", false, media.audio))
        return false;
    if (!media.video.present && !media.audio.present)
        return false;
    if (!generate_sdp(media_path, media, media.sdp))
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
    const std::lock_guard<std::mutex> lock(stream_mutex_);
    if (ffmpeg_pid_ > 0)
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

    std::string video_rtp_url;
    if (stream_video)
        video_rtp_url = make_rtp_url(host, video_ports_.client_rtp);
    std::string audio_rtp_url;
    if (stream_audio)
        audio_rtp_url = make_rtp_url(host, audio_ports_.client_rtp);

    const std::string* video_url = stream_video ? &video_rtp_url : nullptr;
    const std::string* audio_url = stream_audio ? &audio_rtp_url : nullptr;
    const std::string rtp_cname = make_rtp_cname(session_id_);
    const std::vector<std::string> args =
        make_ffmpeg_args(current_media_path_, current_media_, video_url, audio_url, &rtp_cname, true, true);
    log() << "starting ffmpeg: " << join_command(args);

    const pid_t pid = spawn_process(args);
    if (pid < 0) {
        log() << "fork() failed for ffmpeg: " << std::strerror(errno);
        return false;
    }

    ffmpeg_pid_ = pid;
    return true;
}

void Session::stop_streaming() {
    pid_t pid = -1;
    {
        const std::lock_guard<std::mutex> lock(stream_mutex_);
        if (ffmpeg_pid_ <= 0)
            return;
        pid = ffmpeg_pid_;
        ffmpeg_pid_ = -1;
    }

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        log() << "kill(SIGTERM) failed for ffmpeg pid " << pid << ": " << std::strerror(errno);

    wait_for_process(pid, log_prefix() + " ffmpeg");
}

bool Session::handle_request(const std::string& raw_request, bool& should_close) {
    RtspRequest request;
    if (!parse_rtsp_request(raw_request, request))
        return send_response(400, "Bad Request", "0", {}, "");

    log() << request.method;
    const std::string cseq = get_header(request, "cseq");

    if (request.method == "OPTIONS")
        return send_response(200, "OK", cseq, {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN"}}, "");

    if (request.method == "DESCRIBE") {
        std::filesystem::path rel_path;
        if (!parse_media_relative_path(request.uri, rel_path))
            return send_response(400, "Bad Request", cseq, {}, "");

        const auto media_path = media_dir_ / rel_path;
        if (!std::filesystem::exists(media_path))
            return send_response(404, "Not Found", cseq, {}, "");

        const std::string media_uri = strip_track_suffix(request.uri);
        if (!load_media_description(media_path, media_uri))
            return send_response(415, "Unsupported Media Type", cseq, {}, "");

        std::string base = current_media_uri_;
        if (!base.empty() && base.back() != '/')
            base += '/';
        return send_response(
            200,
            "OK",
            cseq,
            {{"Content-Base", base}, {"Content-Type", "application/sdp"}},
            current_media_.sdp);
    }

    if (request.method == "SETUP") {
        const std::string transport = get_header(request, "transport");
        if (transport.empty())
            return send_response(461, "Unsupported Transport", cseq, {}, "");

        std::uint16_t parsed_rtp = 0;
        std::uint16_t parsed_rtcp = 0;
        if (!parse_client_ports(transport, parsed_rtp, parsed_rtcp))
            return send_response(461, "Unsupported Transport", cseq, {}, "");

        const std::string media_uri = strip_track_suffix(request.uri);
        std::filesystem::path rel_path;
        if (!parse_media_relative_path(media_uri, rel_path))
            return send_response(400, "Bad Request", cseq, {}, "");
        const auto media_path = media_dir_ / rel_path;
        if (!std::filesystem::exists(media_path))
            return send_response(404, "Not Found", cseq, {}, "");

        int track_id = -1;
        if (!parse_track_id(request.uri, track_id))
            return send_response(400, "Bad Request", cseq, {}, "");

        if (has_setup_tracks()) {
            const std::string req_session = session_id_only(get_header(request, "session"));
            if (!req_session.empty() && req_session != session_id_text())
                return send_response(454, "Session Not Found", cseq, {}, "");
        }

        if (!current_media_path_.empty() && current_media_path_ != media_path) {
            stop_streaming();
            video_ports_.setup = false;
            audio_ports_.setup = false;
            current_media_ = {};
        }

        if (!load_media_description(media_path, media_uri))
            return send_response(415, "Unsupported Media Type", cseq, {}, "");

        if (!track_id_is_valid(current_media_, track_id))
            return send_response(404, "Not Found", cseq, {}, "");

        TrackPorts* ports = nullptr;
        if (track_id_is_video(current_media_, track_id))
            ports = &video_ports_;
        else if (track_id_is_audio(current_media_, track_id))
            ports = &audio_ports_;
        else
            return send_response(404, "Not Found", cseq, {}, "");
        ports->client_rtp = parsed_rtp;
        ports->client_rtcp = parsed_rtcp;
        ports->setup = true;

        std::ostringstream transport_reply;
        transport_reply << "RTP/AVP;unicast;client_port=" << ports->client_rtp << '-' << ports->client_rtcp
                        << ";server_port=" << ports->server_rtp << '-' << ports->server_rtcp;
        return send_response(
            200,
            "OK",
            cseq,
            {{"Session", session_id_text()}, {"Transport", transport_reply.str()}},
            "");
    }

    if (request.method == "PLAY") {
        if (!has_setup_tracks())
            return send_response(454, "Session Not Found", cseq, {}, "");

        const std::string req_session = session_id_only(get_header(request, "session"));
        if (!req_session.empty() && req_session != session_id_text())
            return send_response(454, "Session Not Found", cseq, {}, "");
        const bool can_play_video = current_media_.video.present && video_ports_.setup && video_ports_.client_rtp != 0;
        const bool can_play_audio = current_media_.audio.present && audio_ports_.setup && audio_ports_.client_rtp != 0;
        if (current_media_path_.empty() || (!can_play_video && !can_play_audio))
            return send_response(455, "Method Not Valid In This State", cseq, {}, "");
        if (!std::filesystem::exists(current_media_path_))
            return send_response(404, "Not Found", cseq, {}, "");

        if (!start_streaming())
            return send_response(500, "Internal Server Error", cseq, {}, "");

        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("Session", session_id_text());
        return send_response(200, "OK", cseq, headers, "");
    }

    if (request.method == "TEARDOWN") {
        std::vector<std::pair<std::string, std::string>> headers;
        if (has_setup_tracks())
            headers.emplace_back("Session", session_id_text());
        stop_streaming();
        should_close = true;
        return send_response(200, "OK", cseq, headers, "");
    }

    return send_response(501, "Not Implemented", cseq, {}, "");
}

void Session::run() {
    std::string pending;
    char buffer[4096];
    bool should_close = false;

    while (!should_close) {
        const ssize_t nread = recv(client_fd_, buffer, sizeof(buffer), 0);
        if (nread == 0)
            break;
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            log() << "recv() failed from " << remote_endpoint_ << ": " << std::strerror(errno);
            break;
        }

        pending.append(buffer, static_cast<std::size_t>(nread));

        while (!should_close) {
            std::string request;
            if (!extract_next_rtsp_request(pending, request))
                break;
            if (!handle_request(request, should_close)) {
                should_close = true;
                break;
            }
        }
    }

    stop_streaming();

    const std::lock_guard<std::mutex> lock(fd_mutex_);
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }
}
