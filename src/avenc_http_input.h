#pragma once
#include <cerrno>
#include <cctype>
#include <chrono>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

// Python's shared SeekableSource owns upstream HTTP, caching and measurements.
// FFmpeg retains responsibility for MP4 parsing and issuing seek/read requests.
class AvencHttpInput {
    pid_t pid_ = -1;
    int control_ = -1;
    FILE *output_ = nullptr;
    bool report_ = false;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::string line() {
        char *buffer = nullptr;
        size_t capacity = 0;
        const ssize_t n = getline(&buffer, &capacity, output_);
        std::string value = n > 0 ? std::string(buffer, static_cast<size_t>(n)) : "";
        free(buffer);
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
public:
    std::string path;
    size_t object_size = 0;
    const size_t *grib_messages_written = nullptr;
    static bool is_url(const std::string &value) {
        std::string prefix = value.substr(0, 8);
        for (char &c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return prefix.rfind("http://", 0) == 0 || prefix.rfind("https://", 0) == 0;
    }
    AvencHttpInput(const std::string &input, const std::string &executable,
                   size_t block_size, double timeout, size_t fallback_limit,
                   bool fallback, bool report) : report_(report), path(input) {
        if (!is_url(input)) return;
        // The Python backend already owns this exact adapter/cache.
        if (const char *resolved = std::getenv("AVENC_RESOLVED_HTTP_INPUT")) {
            if (input == resolved && input.rfind("http://127.0.0.1:", 0) == 0) return;
        }
        std::string helper;
        if (const char *override_path = std::getenv("AVENC_HTTP_HELPER")) helper = override_path;
        if (helper.empty()) {
            std::filesystem::path binary = std::filesystem::absolute(executable);
#ifdef __APPLE__
            char executable_buffer[4096]; uint32_t executable_size = sizeof(executable_buffer);
            if (_NSGetExecutablePath(executable_buffer, &executable_size) == 0) binary = executable_buffer;
#elif defined(__linux__)
            char executable_buffer[4096];
            ssize_t executable_size = readlink("/proc/self/exe", executable_buffer, sizeof(executable_buffer) - 1);
            if (executable_size > 0) { executable_buffer[executable_size] = 0; binary = executable_buffer; }
#endif
            std::filesystem::path base = binary.parent_path();
            auto installed = base / "../share/avenc/source.py";
            if (std::filesystem::exists(installed)) helper = installed.string();
            else helper = AVENC_HTTP_HELPER_PATH;
        }
        if (!std::filesystem::exists(helper)) throw std::runtime_error("HTTP input helper missing: " + helper);
        int input_pipe[2], output_pipe[2];
        if (pipe(input_pipe) || pipe(output_pipe)) throw std::runtime_error("HTTP helper pipe failed");
        std::vector<std::string> args = {"python3", helper, input, "--block-size", std::to_string(block_size),
            "--timeout", std::to_string(timeout), "--fallback-limit", std::to_string(fallback_limit)};
        if (!fallback) args.push_back("--no-http-fallback");
        std::vector<char *> argv;
        for (auto &arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        pid_ = fork();
        if (pid_ == 0) {
            dup2(input_pipe[0], STDIN_FILENO);
            dup2(output_pipe[1], STDOUT_FILENO);
            for (int fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1]}) close(fd);
            execvp(argv[0], argv.data());
            perror("Unable to launch python3 HTTP input helper");
            _exit(127);
        }
        close(input_pipe[0]); close(output_pipe[1]);
        if (pid_ < 0) { close(input_pipe[1]); close(output_pipe[0]); throw std::runtime_error("HTTP helper fork failed"); }
        control_ = input_pipe[1];
        fcntl(control_, F_SETFD, FD_CLOEXEC);
        fcntl(output_pipe[0], F_SETFD, FD_CLOEXEC);
        output_ = fdopen(output_pipe[0], "r");
        path = line();
        if (path.empty()) { finish(); throw std::runtime_error("Unable to open HTTP input (see helper error above)"); }
        try { object_size = std::stoull(line()); }
        catch (...) { finish(); throw std::runtime_error("Invalid HTTP helper size"); }
        std::cerr << "HTTP mode: " << line() << "\n";
    }
    AvencHttpInput(const AvencHttpInput &) = delete;
    ~AvencHttpInput() {
        try { finish(); } catch (const std::exception &error) { std::cerr << error.what() << "\n"; }
    }
    void finish() {
        if (pid_ < 0) return;
        close(control_); control_ = -1;
        std::string stats = line();
        fclose(output_); output_ = nullptr;
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
        if (report_ && !stats.empty()) {
            if (stats.back() == '}') {
                stats.pop_back();
                double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
                stats += ", \"decode_wall_seconds\": " + std::to_string(wall);
                stats += ", \"grib_messages_written\": " + std::to_string(grib_messages_written ? *grib_messages_written : 0) + "}";
            }
            std::cerr << "HTTP_STATS " << stats << "\n";
        }
        if (status != 0) throw std::runtime_error("HTTP source helper failed (see HTTP error above)");
    }
};
