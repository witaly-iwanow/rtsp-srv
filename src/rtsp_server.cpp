#include "rtsp_server.h"
#include "logger.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <netdb.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// max backlog for pending connections
constexpr int kBacklog = 10;
constexpr std::array<const char*, 5> kSupportedExtensions = {".mp4", ".mkv", ".webm", ".mov", ".flv"};

const void* get_in_addr(const sockaddr* sa) {
    if (sa->sa_family == AF_INET)
        return &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr;
    return &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
}

std::string sockaddr_to_string(const sockaddr_storage& addr) {
    char ip[INET6_ADDRSTRLEN];
    const void* raw = get_in_addr(reinterpret_cast<const sockaddr*>(&addr));
    if (!inet_ntop(addr.ss_family, raw, ip, sizeof(ip)))
        return "<unknown>";

    std::uint16_t port = 0;
    if (addr.ss_family == AF_INET)
        port = ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    if (addr.ss_family == AF_INET6)
        port = ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);

    if (addr.ss_family == AF_INET6)
        return std::string("[") + ip + "]:" + std::to_string(port);
    return std::string(ip) + ":" + std::to_string(port);
}

std::string to_lower(std::string value) {
    for (char& ch: value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

bool is_supported_extension(const std::filesystem::path& path) {
    const std::string ext = to_lower(path.extension().string());
    for (auto supported: kSupportedExtensions)
        if (ext == supported)
            return true;
    return false;
}

std::size_t find_media_files(const std::filesystem::path& media_dir) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(media_dir)) {
        const auto& path = entry.path();
        if (is_regular_file(entry.status()) && is_supported_extension(path)) {
            ++count;
            LOG << "Media #" << count << ": " << path.filename();
        }
    }
    return count;
}

// just a little helper to ensure we always clean up
struct ListenerGuard {
    addrinfo* ai = nullptr;
    int fd = -1;

    ~ListenerGuard() {
        if (ai)
            freeaddrinfo(ai);
        if (fd >= 0)
            close(fd);
    }
};

int open_listener(const std::string& host, std::uint16_t port) {
    addrinfo hints {};
    hints.ai_family = (host == "::" ? AF_INET6 : (host == "0.0.0.0" ? AF_INET : AF_UNSPEC));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    ListenerGuard listener;
    const auto service = std::to_string(port);
    const int rv = getaddrinfo(nullptr, service.c_str(), &hints, &listener.ai);
    if (rv != 0 || !listener.ai)
        throw std::runtime_error(std::string("getaddrinfo for ") + host + ":" + service + " failed with " + gai_strerror(rv));

    std::string bind_errors;
    for (addrinfo* p = listener.ai; p; p = p->ai_next) {
        if ((listener.fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;

        // SO_REUSEADDR is omitted on purpose: error out if the port is already in use, to avoid confusion

        if (bind(listener.fd, p->ai_addr, p->ai_addrlen)) {
            const int bind_errno = errno;
            if (bind_errno == EACCES && port < 1024)
                bind_errors += ", privileged port " + std::to_string(port) + " requires sudo or CAP_NET_BIND_SERVICE";
            else
                bind_errors += ", bind() failed with " + std::string(std::strerror(bind_errno));
            close(listener.fd);
            listener.fd = -1;
            continue;
        }

        break;
    }

    if (listener.fd < 0)
        throw std::runtime_error("failed to bind listening socket" + bind_errors);

    if (listen(listener.fd, kBacklog) < 0)
        throw std::runtime_error("listen failed with " + std::string(std::strerror(errno)));

    auto fd = listener.fd;
    listener.fd = -1; // so that it doesn't get closed by the guard
    return fd;
}

}  // namespace

int RtspServer::run() {
    if (!std::filesystem::exists(media_dir_) || !std::filesystem::is_directory(media_dir_)) {
        LOG << "Media directory does not exist or is not a directory: " << media_dir_;
        return -1;
    }

    LOG << "Serving media from " << media_dir_;
    if (find_media_files(media_dir_) == 0) {
        std::string supported_list;
        for (auto ext: kSupportedExtensions)
            supported_list += (supported_list.empty() ? "" : ", ") + std::string(ext);

        LOG << "No supported media files (" << supported_list << ") found in " << media_dir_;

        return -1;
    }

    int listener = -1;
    try {
        listener = open_listener(host_, port_);
    } catch (const std::exception& ex) {
        LOG << "Failed to start listener: " << ex.what();
        return -1;
    }

    LOG << "Listening on " << host_ << ':' << port_;

    while (true) {
        sockaddr_storage client_addr {};
        socklen_t addr_len = sizeof(client_addr);
        const int client_fd = accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            LOG << "accept() failed: " << std::strerror(errno);
            continue;
        }

        const std::string remote_endpoint = sockaddr_to_string(client_addr);
        std::thread([client_fd, remote_endpoint]() {
            LOG << "client connected: " << remote_endpoint;
            close(client_fd);
        }).detach();
    }
}
