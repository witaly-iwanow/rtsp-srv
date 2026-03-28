#include "rtsp_server.h"
#include "logger.h"
#include "session.h"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <csignal>
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
volatile std::sig_atomic_t g_stop_requested = 0;

static void on_stop_signal(int) {
    g_stop_requested = 1;
}

void install_signal_handlers() {
    struct sigaction stop_sa {};
    stop_sa.sa_handler = on_stop_signal;
    sigemptyset(&stop_sa.sa_mask);
    stop_sa.sa_flags = 0;  // do not restart accept(); we want EINTR on Ctrl-C
    if (sigaction(SIGTERM, &stop_sa, nullptr) < 0)
        LOG << "sigaction(SIGTERM) failed: " << std::strerror(errno);
    if (sigaction(SIGINT, &stop_sa, nullptr) < 0)
        LOG << "sigaction(SIGINT) failed: " << std::strerror(errno);

    // to avoid crashing on writing to closed sockets when clients disconnect abruptly
    struct sigaction ignore_sa {};
    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    ignore_sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &ignore_sa, nullptr) < 0)
        LOG << "sigaction(SIGPIPE) failed: " << std::strerror(errno);
}

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
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
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
    for (const auto& entry: std::filesystem::directory_iterator(media_dir)) {
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
    if (rv != 0 || !listener.ai) {
        LOG << "getaddrinfo for " << host << ":" << service << " failed with " << gai_strerror(rv);
        return -1;
    }

    for (addrinfo* p = listener.ai; p; p = p->ai_next) {
        if ((listener.fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;

        int yes = 1;
        if (setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes)))
            LOG << "setsockopt(SO_REUSEADDR) failed";   // not critical, just log and continue

        if (bind(listener.fd, p->ai_addr, p->ai_addrlen)) {
            const int bind_errno = errno;
            if (bind_errno == EACCES && port < 1024)
                LOG << "Privileged port " << port << " requires sudo or CAP_NET_BIND_SERVICE";
            else
                LOG << "bind() failed with " << std::strerror(bind_errno);

            close(listener.fd);
            listener.fd = -1;

            continue;
        }

        break;
    }

    if (listener.fd < 0) {
        LOG << "Failed to bind to " << host << ":" << service;
        return -1;
    }

    if (listen(listener.fd, kBacklog) < 0) {
        LOG << "listen failed with " << std::strerror(errno);
        return -1;
    }

    auto fd = listener.fd;
    listener.fd = -1; // so that it doesn't get closed by the guard
    return fd;
}

}  // namespace

void RtspServer::start_session(int client_fd, const std::string& remote_endpoint) {
    SessionWorker worker;
    worker.session = std::make_unique<Session>(client_fd, remote_endpoint, media_dir_);
    worker.done = std::make_shared<std::atomic<bool>>(false);
    Session* session_ptr = worker.session.get();
    const std::shared_ptr<std::atomic<bool>> done = worker.done;
    worker.thread = std::thread([this, session_ptr, remote_endpoint, done]() {
        const int active = active_sessions_.fetch_add(1) + 1;
        LOG << "client connected: " << remote_endpoint << ", active sessions: " << active;
        session_ptr->run();
        const int after = active_sessions_.fetch_sub(1) - 1;
        LOG << "client disconnected: " << remote_endpoint << ", active sessions: " << after;
        done->store(true, std::memory_order_release);
        sessions_cv_.notify_one();
    });

    const std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.push_back(std::move(worker));
    sessions_cv_.notify_one();
}

void RtspServer::shutdown_sessions() {
    std::vector<SessionWorker> workers;
    {
        const std::lock_guard<std::mutex> lock(sessions_mutex_);
        workers.swap(sessions_);
    }

    if (workers.empty()) {
        LOG << "No active sessions to shutdown";
        return;
    }

    LOG << "Stopping " << workers.size() << " sessions";
    for (auto& worker: workers)
        if (worker.session)
            worker.session->shutdown();
    for (auto& worker: workers)
        if (worker.thread.joinable())
            worker.thread.join();
}

void RtspServer::cleanup_stale_sessions() {
    while (true) {
        std::vector<SessionWorker> finished;
        {
            std::unique_lock<std::mutex> lock(sessions_mutex_);
            sessions_cv_.wait(lock, [this]() {
                if (stop_reaper_)
                    return true;
                for (const auto& worker: sessions_)
                    if (worker.done && worker.done->load(std::memory_order_acquire))
                        return true;
                return false;
            });

            if (stop_reaper_)
                return;

            auto it = sessions_.begin();
            while (it != sessions_.end()) {
                if (it->done && it->done->load(std::memory_order_acquire)) {
                    finished.push_back(std::move(*it));
                    it = sessions_.erase(it);
                    continue;
                }
                ++it;
            }
        }

        for (auto& worker: finished)
            if (worker.thread.joinable())
                worker.thread.join();
    }
}

int RtspServer::run() {
    bool expected = false;
    if (!run_called_.compare_exchange_strong(expected, true)) {
        LOG << "run() can be called only once per RtspServer instance";
        return 1;
    }

    if (!std::filesystem::exists(media_dir_) || !std::filesystem::is_directory(media_dir_)) {
        LOG << "Media directory does not exist or is not a directory: " << media_dir_;
        return 1;
    }

    LOG << "Serving media from " << media_dir_;
    if (find_media_files(media_dir_) == 0) {
        std::string supported_list;
        for (auto ext: kSupportedExtensions)
            supported_list += (supported_list.empty() ? "" : ", ") + std::string(ext);

        LOG << "No supported media files (" << supported_list << ") found in " << media_dir_;

        return 1;
    }

    int listener = open_listener(host_, port_);
    if (listener < 0)
        return 1;

    install_signal_handlers();

    std::thread reaper_thread(&RtspServer::cleanup_stale_sessions, this);

    LOG << "Listening on " << host_ << ':' << port_;
    LOG << "Press Ctrl-C to stop";

    while (!g_stop_requested) {
        sockaddr_storage client_addr {};
        socklen_t addr_len = sizeof(client_addr);
        const int client_fd = accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR && g_stop_requested)
                break;
            if (errno == EINTR)
                continue;

            LOG << "accept() failed: " << std::strerror(errno);
            continue;
        }

        const std::string remote_endpoint = sockaddr_to_string(client_addr);
        start_session(client_fd, remote_endpoint);
    }

    LOG << "Shutting down";
    {
        const std::lock_guard<std::mutex> lock(sessions_mutex_);
        stop_reaper_ = true;
    }
    sessions_cv_.notify_all();
    if (reaper_thread.joinable())
        reaper_thread.join();
    shutdown_sessions();
    close(listener);

    return 0;
}
