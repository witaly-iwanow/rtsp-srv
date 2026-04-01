#pragma once

#include <asio.hpp>

#include "session.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

class RtspServer {
public:
    RtspServer(const std::filesystem::path& media_dir, const std::string& host, std::string service);

    int run();

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
    asio::io_context io_context_;
    Acceptor acceptor_;
    asio::signal_set signals_;
    std::unordered_map<std::uint32_t, Session::Ptr> sessions_;
    std::atomic<std::uint32_t> next_session_id_ {1};
    std::atomic<bool> run_called_ {false};
    bool shutting_down_ = false;
};
