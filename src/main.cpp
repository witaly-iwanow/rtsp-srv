#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "rtsp_server.h"

struct ServerConfig {
    std::filesystem::path media_dir = std::filesystem::current_path();
    std::string host = "0.0.0.0";
    std::string service = "554";
};

void parse_bind(ServerConfig& cfg, const std::string& bind) {
    if (bind.empty())
        throw std::invalid_argument("bind must not be empty");

    if (bind.find(':') == std::string::npos) {
        cfg.service = bind;
        return;
    }

    if (bind.front() == '[') {
        const std::size_t closing = bind.find(']');
        if (closing == std::string::npos || closing + 1 >= bind.size() || bind[closing + 1] != ':')
            throw std::invalid_argument("bind must be <port>, <host:port>, or [<ipv6>]:<port>");
        cfg.host = bind.substr(1, closing - 1);
        if (cfg.host.empty())
            throw std::invalid_argument("host must not be empty");
        cfg.service = bind.substr(closing + 2);
        return;
    }

    const std::size_t sep = bind.rfind(':');
    if (sep == std::string::npos || sep == 0 || sep == bind.size() - 1)
        throw std::invalid_argument("bind must be <port>, <host:port>, or [<ipv6>]:<port>");
    cfg.host = bind.substr(0, sep);
    if (cfg.host.empty())
        throw std::invalid_argument("host must not be empty");
    cfg.service = bind.substr(sep + 1);
}

ServerConfig parse_config(int argc, char** argv) {
    ServerConfig cfg;

    if (argc > 1)
        cfg.media_dir = argv[1];

    if (argc > 2)
        parse_bind(cfg, argv[2]);

    return cfg;
}

void print_usage(const char* exe_name) {
    std::cerr << "Usage: " << exe_name << " [media_directory] [bind]\n"
              << "Defaults:\n"
              << "  media_directory = $PWD\n"
              << "  bind            = 0.0.0.0:554\n"
              << "Accepted bind forms:\n"
              << "  8554\n"
              << "  127.0.0.1:8554\n"
              << "  [::1]:8554\n"
              << "  ::1:8554\n";
}

int main(int argc, char** argv) {
    try {
        const ServerConfig cfg = parse_config(argc, argv);
        RtspServer server(cfg.media_dir, cfg.host, cfg.service);
        return server.run();
    } catch (const std::exception& ex) {
        print_usage(argv[0]);
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
