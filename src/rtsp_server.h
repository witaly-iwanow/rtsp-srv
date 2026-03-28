#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

class RtspServer {
public:
    RtspServer(const std::filesystem::path& media_dir, const std::string& host, std::uint16_t port):
        media_dir_(media_dir), host_(host), port_(port) {};

    int run();

private:
    std::filesystem::path media_dir_;
    std::string host_;
    std::uint16_t port_;
};
