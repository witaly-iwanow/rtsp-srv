#include "session.h"
#include "logger.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::string to_lower(std::string value) {
    for (char& ch: value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
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
            std::string raw_value = line.substr(std::strlen(kContentLength));
            const std::size_t first = raw_value.find_first_not_of(" \t");
            if (first == std::string::npos)
                return 0;
            const std::size_t last = raw_value.find_last_not_of(" \t");
            raw_value = raw_value.substr(first, last - first + 1);
            try {
                return static_cast<std::size_t>(std::stoul(raw_value));
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

std::string parse_rtsp_method(const std::string& request) {
    const std::size_t line_end = request.find("\r\n");
    const std::string first_line = request.substr(0, line_end);
    const std::size_t sep = first_line.find(' ');
    if (sep == std::string::npos || sep == 0)
        return "UNKNOWN";
    return first_line.substr(0, sep);
}

}  // namespace

Session::~Session() {
    const std::lock_guard<std::mutex> lock(fd_mutex_);
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }
}

void Session::shutdown() {
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

void Session::run() {
    std::string pending;
    char buffer[4096];

    while (true) {
        const ssize_t nread = recv(client_fd_, buffer, sizeof(buffer), 0);
        if (nread == 0)
            break;
        if (nread < 0) {
            LOG << "recv() failed from " << remote_endpoint_ << ": " << std::strerror(errno);
            break;
        }

        pending.append(buffer, static_cast<std::size_t>(nread));

        while (true) {
            std::string request;
            if (!extract_next_rtsp_request(pending, request))
                break;
            LOG << parse_rtsp_method(request);
        }
    }

    const std::lock_guard<std::mutex> lock(fd_mutex_);
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }
}
