#pragma once

#include <asio.hpp>

#include "port_registry.h"
#include "session.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>

// Entry point for the RTSP server. Accepts TCP connections, dispatches sessions,
// and manages the media thread pool.
class RtspServer {
public:
    RtspServer(const std::filesystem::path& media_dir, const std::string& host, const std::string& service, std::size_t media_threads = 0);

    [[nodiscard]] int run();

private:
    using Acceptor = asio::ip::tcp::acceptor;
    using Socket = asio::ip::tcp::socket;

    bool open_acceptor();
    void start_accept();
    void handle_accept(asio::error_code ec, Socket socket);
    void handle_stop_signal(asio::error_code ec, int signal_number);
    void shutdown();
    void shutdown_sessions();
    void register_session(Session::Ptr session);
    void on_session_closed(std::uint32_t session_id, const std::string& remote_endpoint);

    std::filesystem::path media_dir_;
    std::string host_;
    std::string service_;
    std::size_t media_threads_;
    asio::io_context io_context_;
    asio::thread_pool media_pool_;
    Acceptor acceptor_;
    asio::signal_set signals_;
    PortRegistry port_registry_;
    std::unordered_map<std::uint32_t, Session::Ptr> sessions_;
    std::atomic<std::uint32_t> next_session_id_{1};
    std::atomic<bool> run_called_{false};
    bool shutting_down_ = false;
};
