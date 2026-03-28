#include "session.h"
#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
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

std::atomic<std::uint64_t> g_session_counter {1};

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
    const std::size_t pos = uri.find("/trackID=");
    if (pos == std::string::npos)
        return uri;
    return uri.substr(0, pos);
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

std::string make_sdp(const std::string& media_name) {
    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 0 0 IN IP4 127.0.0.1\r\n"
        << "s=" << media_name << "\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n"
        << "m=video 0 RTP/AVP 96\r\n"
        << "a=rtpmap:96 H264/90000\r\n"
        << "a=control:trackID=0\r\n";
    return sdp.str();
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

std::string Session::make_session_id() {
    const std::uint64_t id = g_session_counter.fetch_add(1, std::memory_order_relaxed);
    return std::to_string(id);
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

bool Session::start_streaming() {
    const std::lock_guard<std::mutex> lock(stream_mutex_);
    if (ffmpeg_pid_ > 0)
        return true;
    if (current_media_path_.empty() || client_rtp_port_ == 0)
        return false;

    const std::string host = endpoint_host(remote_endpoint_);
    if (host.empty())
        return false;
    const std::string rtp_url = make_rtp_url(host, client_rtp_port_);
    LOG << "Starting ffmpeg: -re -stream_loop -1 -i " << current_media_path_
        << " -an -map 0:v:0 -c:v libx264 -profile:v high -preset faster -crf 23 -pix_fmt yuv420p"
        << " -f rtp -payload_type 96 " << rtp_url;

    const pid_t pid = fork();
    if (pid < 0) {
        LOG << "fork() failed for ffmpeg: " << std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        execlp(
            "ffmpeg",
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-re",
            "-stream_loop",
            "-1",
            "-i",
            current_media_path_.c_str(),
            "-an",
            "-map",
            "0:v:0",
            "-c:v",
            "libx264",
            "-profile:v",
            "high",
            "-preset",
            "faster",
            "-crf",
            "23",
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rtp",
            "-payload_type",
            "96",
            rtp_url.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
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
        LOG << "kill(SIGTERM) failed for ffmpeg pid " << pid << ": " << std::strerror(errno);

    int status = 0;
    const pid_t waited = waitpid(pid, &status, 0);
    if (waited < 0) {
        if (errno != ECHILD)
            LOG << "waitpid() failed for ffmpeg pid " << pid << ": " << std::strerror(errno);
        return;
    }

    if (WIFEXITED(status))
        LOG << "ffmpeg exited with code " << WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        LOG << "ffmpeg terminated by signal " << WTERMSIG(status);
}

bool Session::handle_request(const std::string& raw_request, bool& should_close) {
    RtspRequest request;
    if (!parse_rtsp_request(raw_request, request))
        return send_response(400, "Bad Request", "0", {}, "");

    LOG << request.method;
    const std::string cseq = get_header(request, "cseq");

    if (request.method == "OPTIONS")
        return send_response(200, "OK", cseq, {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN"}}, "");

    if (request.method == "DESCRIBE") {
        std::filesystem::path rel_path;
        if (!parse_media_relative_path(request.uri, rel_path))
            return send_response(400, "Bad Request", cseq, {}, "");

        const std::filesystem::path media_path = media_dir_ / rel_path;
        if (!std::filesystem::exists(media_path))
            return send_response(404, "Not Found", cseq, {}, "");

        current_media_uri_ = strip_track_suffix(request.uri);
        const std::string sdp = make_sdp(media_path.filename().string());
        std::string base = current_media_uri_;
        if (!base.empty() && base.back() != '/')
            base += '/';
        return send_response(
            200,
            "OK",
            cseq,
            {{"Content-Base", base}, {"Content-Type", "application/sdp"}},
            sdp);
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
        if (!std::filesystem::exists(media_dir_ / rel_path))
            return send_response(404, "Not Found", cseq, {}, "");

        if (session_id_.empty())
            session_id_ = make_session_id();
        current_media_uri_ = media_uri;
        current_media_path_ = media_dir_ / rel_path;
        client_rtp_port_ = parsed_rtp;
        client_rtcp_port_ = parsed_rtcp;

        std::ostringstream transport_reply;
        transport_reply << "RTP/AVP;unicast;client_port=" << client_rtp_port_ << '-' << client_rtcp_port_
                        << ";server_port=" << server_rtp_port_ << '-' << server_rtcp_port_;
        return send_response(
            200,
            "OK",
            cseq,
            {{"Session", session_id_}, {"Transport", transport_reply.str()}},
            "");
    }

    if (request.method == "PLAY") {
        if (session_id_.empty())
            return send_response(454, "Session Not Found", cseq, {}, "");

        const std::string req_session = session_id_only(get_header(request, "session"));
        if (!req_session.empty() && req_session != session_id_)
            return send_response(454, "Session Not Found", cseq, {}, "");
        if (current_media_path_.empty() || client_rtp_port_ == 0)
            return send_response(455, "Method Not Valid In This State", cseq, {}, "");
        if (!std::filesystem::exists(current_media_path_))
            return send_response(404, "Not Found", cseq, {}, "");
        if (!start_streaming())
            return send_response(500, "Internal Server Error", cseq, {}, "");

        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("Session", session_id_);
        if (!current_media_uri_.empty())
            headers.emplace_back("RTP-Info", "url=" + current_media_uri_ + "/trackID=0;seq=0;rtptime=0");
        return send_response(200, "OK", cseq, headers, "");
    }

    if (request.method == "TEARDOWN") {
        std::vector<std::pair<std::string, std::string>> headers;
        if (!session_id_.empty())
            headers.emplace_back("Session", session_id_);
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
            LOG << "recv() failed from " << remote_endpoint_ << ": " << std::strerror(errno);
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
