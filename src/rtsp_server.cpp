#include "rtsp_server.h"
#include "logger.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <csignal>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using asio::ip::tcp;

constexpr std::array<const char*, 5> kSupportedExtensions = {".mp4", ".mkv", ".webm", ".mov", ".flv"};

std::size_t default_media_threads() {
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    return std::max<std::size_t>(2, hardware_threads == 0 ? 4 : hardware_threads);
}

void install_sigpipe_handler() {
    struct sigaction ignore_sa {};
    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    ignore_sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &ignore_sa, nullptr) < 0)
        LOG << "sigaction(SIGPIPE) failed: " << std::strerror(errno);
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

bool is_wildcard_host(const std::string& host) {
    return host == "0.0.0.0" || host == "::";
}

bool is_loopback_host(const std::string& host) {
    return host == "127.0.0.1" || host == "::1" || to_lower(host) == "localhost";
}

std::string endpoint_to_string(const tcp::endpoint& endpoint) {
    const std::string host = endpoint.address().to_string();
    if (endpoint.address().is_v6())
        return "[" + host + "]:" + std::to_string(endpoint.port());
    return host + ":" + std::to_string(endpoint.port());
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

std::vector<tcp::endpoint> resolve_bind_endpoints(
    asio::io_context& io_context,
    const std::string& host,
    const std::string& service) {
    tcp::resolver resolver(io_context);
    asio::error_code ec;
    const std::string node = is_wildcard_host(host) ? "" : host;
    const tcp::resolver::flags flags = is_wildcard_host(host) ? tcp::resolver::passive : tcp::resolver::flags();
    const auto results = resolver.resolve(node, service, flags, ec);
    if (ec) {
        const std::string display_host = node.empty() ? host : node;
        LOG << "resolve(" << display_host << ":" << service << ") failed: " << ec.message();
        return {};
    }

    std::vector<tcp::endpoint> endpoints;
    for (const auto& result: results)
        endpoints.push_back(result.endpoint());
    return endpoints;
}

}  // namespace

RtspServer::RtspServer(const std::filesystem::path& media_dir, const std::string& host, std::string service):
    media_dir_(media_dir),
    host_(host),
    service_(std::move(service)),
    media_pool_(default_media_threads()),
    acceptor_(io_context_),
    signals_(io_context_, SIGINT, SIGTERM) {}

bool RtspServer::open_acceptor() {
    const std::vector<tcp::endpoint> endpoints = resolve_bind_endpoints(io_context_, host_, service_);
    if (endpoints.empty()) {
        LOG << "Failed to resolve bind address " << host_ << ":" << service_;
        return false;
    }

    if (is_loopback_host(host_))
        LOG << "Binding to loopback only (" << host_ << "). Remote clients will not connect; feel free to shoot yourself in the foot.";

    asio::error_code ec;
    for (const tcp::endpoint& endpoint: endpoints) {
        acceptor_.close(ec);
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            LOG << "open(" << endpoint_to_string(endpoint) << ") failed: " << ec.message();
            continue;
        }

        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            LOG << "set_option(SO_REUSEADDR) failed: " << ec.message();
            acceptor_.close();
            continue;
        }

        if (endpoint.protocol() == tcp::v6() && host_ == "::") {
            acceptor_.set_option(asio::ip::v6_only(false), ec);
            if (ec) {
                LOG << "set_option(IPV6_V6ONLY=0) failed: " << ec.message();
                acceptor_.close();
                continue;
            }
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            LOG << "bind(" << endpoint_to_string(endpoint) << ") failed: " << ec.message();
            acceptor_.close();
            continue;
        }

        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            LOG << "listen() failed: " << ec.message();
            acceptor_.close();
            continue;
        }

        return true;
    }

    LOG << "Failed to bind to " << host_ << ":" << service_;
    return false;
}

void RtspServer::start_accept() {
    acceptor_.async_accept([this](asio::error_code ec, Socket socket) {
        handle_accept(ec, std::move(socket));
    });
}

void RtspServer::handle_accept(asio::error_code ec, Socket socket) {
    if (shutting_down_)
        return;
    if (ec) {
        if (ec != asio::error::operation_aborted)
            LOG << "accept() failed: " << ec.message();
        if (!shutting_down_)
            start_accept();
        return;
    }

    asio::error_code endpoint_ec;
    const std::string remote_endpoint = endpoint_to_string(socket.remote_endpoint(endpoint_ec));
    if (endpoint_ec)
        LOG << "remote_endpoint() failed: " << endpoint_ec.message();

    const auto session_id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
    register_session(std::make_shared<Session>(
        std::move(socket),
        remote_endpoint,
        media_dir_,
        media_pool_.get_executor(),
        session_id,
        [this](std::uint32_t id, const std::string& remote) { on_session_closed(id, remote); }));

    start_accept();
}

void RtspServer::handle_stop_signal(asio::error_code ec, int) {
    if (ec == asio::error::operation_aborted)
        return;
    if (ec) {
        LOG << "signal handling failed: " << ec.message();
        return;
    }
    shutdown();
}

void RtspServer::shutdown_sessions() {
    if (sessions_.empty()) {
        LOG << "No active sessions to shutdown";
        return;
    }

    LOG << "Stopping " << sessions_.size() << " sessions";
    for (auto& [_, session]: sessions_)
        session->shutdown();
}

void RtspServer::shutdown() {
    if (shutting_down_)
        return;

    shutting_down_ = true;
    LOG << "Shutting down";

    asio::error_code ec;
    acceptor_.cancel(ec);
    acceptor_.close(ec);
    signals_.cancel(ec);
    shutdown_sessions();
}

void RtspServer::register_session(Session::Ptr session) {
    const std::uint32_t session_id = session->session_id();
    const std::string remote_endpoint = session->remote_endpoint();
    sessions_.emplace(session_id, session);
    session->log() << "client connected: " << remote_endpoint << ", active sessions: " << sessions_.size();
    session->start();
}

void RtspServer::on_session_closed(std::uint32_t session_id, const std::string& remote_endpoint) {
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end())
        return;

    Session::Ptr session = std::move(it->second);
    sessions_.erase(it);
    session->log() << "client disconnected: " << remote_endpoint << ", active sessions: " << sessions_.size();

    if (shutting_down_ && sessions_.empty())
        io_context_.stop();
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

    if (!open_acceptor())
        return 1;

    install_sigpipe_handler();

    LOG << "Listening on " << host_ << ':' << service_;
    LOG << "Press Ctrl-C to stop";
    signals_.async_wait([this](asio::error_code ec, int signal_number) {
        handle_stop_signal(ec, signal_number);
    });
    start_accept();

    io_context_.run();
    media_pool_.join();

    return 0;
}
