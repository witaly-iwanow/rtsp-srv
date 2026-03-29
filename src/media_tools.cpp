#include "media_tools.h"
#include "logger.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return std::tolower(c);
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

bool is_passthrough_video_codec(const std::string& codec_name) {
    return codec_name == "hevc" || codec_name == "h264" || codec_name == "mpeg2video" || codec_name == "vp8"
        || codec_name == "vp9";
}

bool is_passthrough_audio_codec(const std::string& codec_name) {
    return codec_name == "opus" || codec_name == "aac" || codec_name == "mp1" || codec_name == "mp2"
        || codec_name == "mp3";
}

std::string quote_arg(const std::string& value) {
    if (value.find_first_of(" \t\n'\"\\") == std::string::npos)
        return value;

    std::string quoted = "'";
    for (const char c: value) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += '\'';
    return quoted;
}

[[noreturn]] void exec_command_args(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg: args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
}

bool run_command_capture(const std::vector<std::string>& args, std::string& output) {
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) < 0) {
        LOG << "pipe() failed: " << std::strerror(errno);
        return false;
    }

    const pid_t pid = spawn_process(args, pipe_fds[1], pipe_fds[1]);
    close(pipe_fds[1]);
    if (pid < 0) {
        close(pipe_fds[0]);
        LOG << "fork() failed for command: " << std::strerror(errno);
        return false;
    }

    output.clear();
    char buffer[4096];
    while (true) {
        const ssize_t nread = read(pipe_fds[0], buffer, sizeof(buffer));
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            close(pipe_fds[0]);
            LOG << "read() failed while capturing command output: " << std::strerror(errno);
            wait_for_process(pid, args.front(), false);
            return false;
        }
        if (nread == 0)
            break;
        output.append(buffer, static_cast<std::size_t>(nread));
    }
    close(pipe_fds[0]);

    return wait_for_process(pid, args.front(), false);
}

}  // namespace

std::string join_command(const std::vector<std::string>& args) {
    std::ostringstream command;
    bool first = true;
    for (const std::string& arg: args) {
        if (!first)
            command << ' ';
        command << quote_arg(arg);
        first = false;
    }
    return command.str();
}

pid_t spawn_process(const std::vector<std::string>& args, int stdout_fd, int stderr_fd) {
    const pid_t pid = fork();
    if (pid != 0)
        return pid;

    if (stdout_fd >= 0)
        dup2(stdout_fd, STDOUT_FILENO);
    if (stderr_fd >= 0)
        dup2(stderr_fd, STDERR_FILENO);
    exec_command_args(args);
}

bool wait_for_process(pid_t pid, const std::string& process_name, bool log_exit) {
    int status = 0;
    const pid_t waited = waitpid(pid, &status, 0);
    if (waited < 0) {
        if (errno != ECHILD)
            LOG << "waitpid() failed for " << process_name << " pid " << pid << ": " << std::strerror(errno);
        return false;
    }

    if (WIFEXITED(status)) {
        if (log_exit)
            LOG << process_name << " exited with code " << WEXITSTATUS(status);
        return WEXITSTATUS(status) == 0;
    }
    if (WIFSIGNALED(status)) {
        if (log_exit)
            LOG << process_name << " terminated by signal " << WTERMSIG(status);
        return false;
    }
    return false;
}

bool probe_track(const std::filesystem::path& media_path, const char* selector, bool is_video, MediaTrack& track) {
    const std::vector<std::string> args {
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        selector,
        "-show_entries",
        "stream=codec_name,channels,sample_rate",
        "-of",
        "default=noprint_wrappers=1:nokey=0",
        media_path.string(),
    };

    std::string output;
    if (!run_command_capture(args, output)) {
        LOG << "ffprobe failed: " << join_command(args);
        return false;
    }

    track = {};
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        const std::size_t sep = line.find('=');
        if (sep == std::string::npos)
            continue;

        const std::string key = trim(line.substr(0, sep));
        const std::string value = trim(line.substr(sep + 1));
        if (key == "codec_name") {
            track.codec_name = to_lower(value);
            track.present = !track.codec_name.empty();
        } else if (key == "channels" && !value.empty()) {
            track.channels = std::stoi(value);
        } else if (key == "sample_rate" && !value.empty()) {
            track.sample_rate = std::stoi(value);
        }
    }

    if (!track.present)
        return true;

    track.copy = is_video ? is_passthrough_video_codec(track.codec_name) : is_passthrough_audio_codec(track.codec_name);
    return true;
}

std::vector<std::string> make_ffmpeg_args(
    const std::filesystem::path& media_path,
    const MediaDescription& media,
    const std::string* video_rtp_url,
    const std::string* audio_rtp_url,
    const std::string* rtp_cname,
    bool realtime,
    bool loop_input,
    const std::string* sdp_file) {
    std::vector<std::string> args {"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin"};
    if (realtime)
        args.emplace_back("-re");
    if (loop_input) {
        args.emplace_back("-stream_loop");
        args.emplace_back("-1");
    }
    args.emplace_back("-i");
    args.emplace_back(media_path.string());
    if (sdp_file != nullptr) {
        args.emplace_back("-sdp_file");
        args.emplace_back(*sdp_file);
    } else if (video_rtp_url != nullptr || audio_rtp_url != nullptr) {
        // Without an explicit sink ffmpeg prints generated SDP to stdout for RTP outputs.
        args.emplace_back("-sdp_file");
        args.emplace_back("/dev/null");
    }

    if (video_rtp_url != nullptr && media.video.present) {
        args.emplace_back("-map");
        args.emplace_back("0:v:0");
        args.emplace_back("-c:v");
        if (media.video.copy) {
            args.emplace_back("copy");
        } else {
            args.emplace_back("libx264");
            args.emplace_back("-profile:v");
            args.emplace_back("high");
            args.emplace_back("-preset");
            args.emplace_back("faster");
            args.emplace_back("-crf");
            args.emplace_back("23");
            args.emplace_back("-pix_fmt");
            args.emplace_back("yuv420p");
        }
        if (rtp_cname != nullptr) {
            args.emplace_back("-cname");
            args.emplace_back(*rtp_cname);
        }
        args.emplace_back("-f");
        args.emplace_back("rtp");
        args.emplace_back(*video_rtp_url);
    }

    if (audio_rtp_url != nullptr && media.audio.present) {
        args.emplace_back("-map");
        args.emplace_back("0:a:0");
        args.emplace_back("-c:a");
        if (media.audio.copy) {
            args.emplace_back("copy");
        } else {
            args.emplace_back("libopus");
            args.emplace_back("-ac");
            args.emplace_back("2");
            args.emplace_back("-ar");
            args.emplace_back("48000");
        }
        if (rtp_cname != nullptr) {
            args.emplace_back("-cname");
            args.emplace_back(*rtp_cname);
        }
        args.emplace_back("-f");
        args.emplace_back("rtp");
        args.emplace_back(*audio_rtp_url);
    }

    return args;
}
