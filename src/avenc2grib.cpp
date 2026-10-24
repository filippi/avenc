#include "avenc_transform.h"
#include "avenc_pixel_layout.h"
#include "avenc_version.h"
#include "avenc_grib_time.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <chrono>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <eccodes.h>
#include <zlib.h>

#include "avenc_subset_decode.h"
#include "avenc_http_input.h"

namespace {

struct Options {
    std::string input_mp4;
    std::string output_path;
    std::string group_selector;
    bool list = false;
    bool stats = false;
    bool head = false;
    bool verbose = false;
    bool http_stats = false;
    bool http_fallback = true;
    size_t http_block_size = 1024 * 1024;
    size_t http_fallback_limit = 1024ULL * 1024 * 1024;
    double http_timeout = 30;
    int stream_id = -1;
    int ni_start = 0;
    int nj_start = 0;
    int width = -1;
    int height = -1;
    int frame_start = 0;
    int frame_count = -1;
    size_t page_size_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL; // default: 8 GiB
    std::string zarr_compress = "none"; // none|gzip
    std::string zarr_out;
};

struct StreamInfo {
    int stream_index = 0;
    size_t frame_count = 0;
    int bits = 8;
    int storage_shift = 0;
    double min_value = 0.0;
    double max_value = 1.0;
    bool apply_log2 = false;
    bool apply_asinh = false;
    bool apply_shifted_log = false;
    double transform_scale = 1.0, transform_origin = 0.0, transform_span = 1.0;
    double log_offset = 1.0;
    std::vector<uint8_t> shared_bitmap;
    std::string template_b64;
    int encoded_nx = 0;
    int encoded_ny = 0;
    int crop_nx = 0;
    int crop_ny = 0;
};

struct RecordInfo {
    size_t order = 0;
    int stream_index = 0;
    size_t frame_index = 0;
    int file_index = -1;
    size_t message_index = 0;
    std::string short_name;
    std::string type_of_level;
    std::string level;
    std::string data_date;
    std::string data_time;
    std::string step_range;
    std::string timestamp;
    std::string ensemble_number;
    std::string perturbation_number;
    double frame_min = std::numeric_limits<double>::quiet_NaN();
    double frame_max = std::numeric_limits<double>::quiet_NaN();
    bool bitmap_present = false;
    std::string template_b64;
    int bits_per_value = -1;
    int binary_scale_shift = 0;
};

struct ReconPayload {
    int nx = 0;
    int ny = 0;
    std::vector<StreamInfo> streams;
    std::vector<RecordInfo> records;
};

struct GroupInfo {
    std::string alias;
    std::string display_name;
    int stream_index = -1;
    size_t frame_count = 0;
    size_t record_count = 0;
    std::string short_name;
    std::string type_of_level;
    size_t unique_levels = 0;
    size_t unique_steps = 0;
    int bits = 8;
    int storage_shift = 0;
    double min_value = 0.0;
    double max_value = 0.0;
};

std::string serialize_recon_payload(const ReconPayload &recon);

[[noreturn]] void usage(const char *prog, int status = 1) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " [--verbose] [--group <name|stream:idx[,..]>] --list <input.mp4>\n"
        << "  " << prog << " [--verbose] --stats <input.mp4>\n"
        << "  " << prog << " [--verbose] --head <input.mp4>\n"
        << "  " << prog << " [--verbose] [--group <name|stream:idx[,..]>] <input.mp4> <output.grib|output.mp4>\n\n"
        << "  " << prog << " [--verbose] [--zarr-compress none|gzip] [--group <name|stream:idx[,..]>] <input.mp4> <output.zarr>\n\n"
        << "  " << prog << " [--verbose] [--group <name|stream:idx[,..]>|--stream-id <int>]"
        << " [--ni-start N --nj-start N --width W --height H]"
        << " [--frame-start N --frame-count N] [--zarr-compress none|gzip] --zarr-out <subset.zarr> <input.mp4>\n\n"
        << "  " << prog << " [--verbose] --stream-id <int> [--ni-start N --nj-start N --width W --height H]\n"
        << "      [--frame-start N --frame-count N] <input.mp4> > subset.f32\n"
        << "  " << prog << " [--verbose] [--page-size <bytes|KiB|MiB|GiB>] <input.mp4> <output.grib>\n\n"
        << "HTTP inputs: --http-stats --no-http-fallback --http-block-size 1MiB\n"
        << "             --http-timeout 30 --http-fallback-limit 1GiB\n\n"
        << "Examples:\n"
        << "  " << prog << " --list input.mp4\n"
        << "  " << prog << " --stats input.mp4\n"
        << "  " << prog << " --head input.mp4\n"
        << "  " << prog << " --group q --list input.mp4\n"
        << "  " << prog << " --group q input.mp4 q_subset.mp4\n"
        << "  " << prog << " --group q input.mp4 q_subset.grib\n"
        << "  " << prog << " --zarr-compress gzip input.mp4 out.zarr\n"
        << "  " << prog << " --group q --ni-start 64 --nj-start 64 --width 128 --height 128"
        << " --frame-start 10 --frame-count 20 --zarr-compress gzip --zarr-out outChunk.zarr"
        << " input.mp4\n"
        << "  " << prog << " --page-size 8GiB input.mp4 output.grib\n"
        << "  " << prog << " --stream-id 0 --ni-start 64 --nj-start 64 --width 128 --height 128"
        << " --frame-start 10 --frame-count 20 input.mp4 > chunk.f32\n";
    std::exit(status);
}

std::string format_hms(double sec) {
    if (!std::isfinite(sec) || sec < 0.0) sec = 0.0;
    const long s = static_cast<long>(std::llround(sec));
    const long h = s / 3600;
    const long m = (s % 3600) / 60;
    const long r = s % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", h, m, r);
    return std::string(buf);
}

class ProgressBar {
public:
    ProgressBar(std::string label, uint64_t total, bool enabled)
        : label_(std::move(label)),
          total_(std::max<uint64_t>(1, total)),
          enabled_(enabled),
          start_(std::chrono::steady_clock::now()),
          last_draw_(start_) {
        if (enabled_) draw(true);
    }

    ~ProgressBar() {
        if (enabled_ && !finished_ && last_line_len_ > 0) std::cerr << "\n";
    }

    void update(uint64_t done_units) {
        if (!enabled_) return;
        done_ = std::min(done_units, total_);
        draw(false);
    }

    void increment(uint64_t delta = 1) {
        if (!enabled_) return;
        update(done_ + delta);
    }

    void finish() {
        if (!enabled_ || finished_) return;
        if (done_ < total_) {
            done_ = total_;
            draw(true);
        }
        std::cerr << "\n";
        finished_ = true;
    }

private:
    void draw(bool force) {
        if (!enabled_) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && done_ < total_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_draw_).count() < 120) {
            return;
        }
        last_draw_ = now;

        const double progress = static_cast<double>(done_) / static_cast<double>(total_);
        constexpr int bar_w = 30;
        const int fill = static_cast<int>(std::llround(progress * bar_w));
        std::string bar(static_cast<size_t>(bar_w), '-');
        for (int i = 0; i < std::min(fill, bar_w); ++i) bar[static_cast<size_t>(i)] = '#';

        const double elapsed = std::chrono::duration<double>(now - start_).count();
        double eta_sec = 0.0;
        if (done_ > 0 && done_ < total_) {
            eta_sec = elapsed * static_cast<double>(total_ - done_) / static_cast<double>(done_);
        }

        std::ostringstream line;
        line << label_ << " [" << bar << "] "
             << std::fixed << std::setprecision(1) << (progress * 100.0) << "% "
             << "(" << done_ << "/" << total_ << ") ETA " << format_hms(eta_sec);

        const std::string txt = line.str();
        std::cerr << "\r" << txt;
        if (txt.size() < last_line_len_) std::cerr << std::string(last_line_len_ - txt.size(), ' ');
        std::cerr.flush();
        last_line_len_ = txt.size();
    }

    std::string label_;
    uint64_t total_ = 1;
    uint64_t done_ = 0;
    bool enabled_ = false;
    bool finished_ = false;
    size_t last_line_len_ = 0;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_draw_;
};

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::string lower_copy(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool ends_with_icase(const std::string &s, const std::string &suffix) {
    if (s.size() < suffix.size()) return false;
    const std::string tail = s.substr(s.size() - suffix.size());
    return lower_copy(tail) == lower_copy(suffix);
}

bool is_integer_token(const std::string &s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

size_t parse_size_bytes(const std::string &text) {
    std::string s = lower_copy(trim(text));
    if (s.empty()) throw std::runtime_error("empty page size");

    size_t pos = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos == 0) throw std::runtime_error("invalid page size: " + text);
    const std::string num_s = s.substr(0, pos);
    std::string suf = trim(s.substr(pos));
    if (!suf.empty() && suf[0] == ' ') suf = trim(suf);

    uint64_t base = 0;
    try {
        base = static_cast<uint64_t>(std::stoull(num_s));
    } catch (...) {
        throw std::runtime_error("invalid page size number: " + text);
    }

    uint64_t mul = 1;
    if (suf.empty() || suf == "b") mul = 1ULL;
    else if (suf == "k" || suf == "kb" || suf == "kib") mul = 1024ULL;
    else if (suf == "m" || suf == "mb" || suf == "mib") mul = 1024ULL * 1024ULL;
    else if (suf == "g" || suf == "gb" || suf == "gib") mul = 1024ULL * 1024ULL * 1024ULL;
    else if (suf == "t" || suf == "tb" || suf == "tib") mul = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    else throw std::runtime_error("unsupported page size suffix in: " + text);

    if (base == 0ULL || base > (std::numeric_limits<uint64_t>::max() / mul)) {
        throw std::runtime_error("page size out of range: " + text);
    }
    const uint64_t bytes = base * mul;
    if (bytes == 0ULL) throw std::runtime_error("page size must be > 0");
    return static_cast<size_t>(bytes);
}

bool subset_mode_requested(const Options &opts) {
    return (opts.stream_id >= 0) ||
           (opts.ni_start != 0) || (opts.nj_start != 0) ||
           (opts.width > 0) || (opts.height > 0) ||
           (opts.frame_start != 0) || (opts.frame_count > 0);
}

void parse_args(int argc, char **argv, Options &opts) {
    if (argc < 2) usage(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--http-stats") {
            opts.http_stats = true;
        } else if (a == "--no-http-fallback") {
            opts.http_fallback = false;
        } else if (a == "--http-block-size") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.http_block_size = parse_size_bytes(argv[++i]);
        } else if (a == "--http-fallback-limit") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.http_fallback_limit = parse_size_bytes(argv[++i]);
        } else if (a == "--http-timeout") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.http_timeout = std::stod(argv[++i]);
            if (!(opts.http_timeout > 0)) throw std::runtime_error("HTTP timeout must be positive");
        } else if (a == "--verbose") {
            opts.verbose = true;
        } else if (a == "--list") {
            opts.list = true;
        } else if (a == "--stats") {
            opts.stats = true;
        } else if (a == "--head") {
            opts.head = true;
        } else if (a == "--group") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.group_selector = argv[++i];
        } else if (a == "--stream-id") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.stream_id = std::stoi(argv[++i]);
        } else if (a == "--ni-start") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.ni_start = std::stoi(argv[++i]);
        } else if (a == "--nj-start") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.nj_start = std::stoi(argv[++i]);
        } else if (a == "--width") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.width = std::stoi(argv[++i]);
        } else if (a == "--height") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.height = std::stoi(argv[++i]);
        } else if (a == "--frame-start") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.frame_start = std::stoi(argv[++i]);
        } else if (a == "--frame-count") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.frame_count = std::stoi(argv[++i]);
        } else if (a == "--page-size") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.page_size_bytes = parse_size_bytes(argv[++i]);
        } else if (a == "--zarr-compress") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.zarr_compress = lower_copy(trim(argv[++i]));
        } else if (a == "--zarr-out") {
            if (i + 1 >= argc) usage(argv[0]);
            opts.zarr_out = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            usage(argv[0]);
        } else if (opts.input_mp4.empty()) {
            opts.input_mp4 = a;
        } else if (opts.output_path.empty()) {
            opts.output_path = a;
        } else {
            usage(argv[0]);
        }
    }

    if (opts.input_mp4.empty()) usage(argv[0]);
    const bool subset_mode = subset_mode_requested(opts);
    const bool subset_zarr_mode = !trim(opts.zarr_out).empty();
    const bool info_mode = (opts.list || opts.stats || opts.head);
    if (!info_mode && !subset_mode && opts.output_path.empty() && !subset_zarr_mode) usage(argv[0]);
    if (info_mode && (!opts.output_path.empty() || subset_zarr_mode)) usage(argv[0]);
    if ((opts.list && opts.stats) || (opts.list && opts.head) || (opts.stats && opts.head)) {
        throw std::runtime_error("--list, --stats and --head are mutually exclusive");
    }
    if (!opts.output_path.empty() && subset_zarr_mode) {
        throw std::runtime_error("Use either positional output path or --zarr-out, not both");
    }
    if (!opts.output_path.empty() && opts.output_path == opts.input_mp4) {
        throw std::runtime_error("input and output path must differ");
    }
    if (subset_mode && !subset_zarr_mode) {
        if (opts.list) throw std::runtime_error("--list is incompatible with subset float32 mode");
        if (opts.stats || opts.head) {
            throw std::runtime_error("--stats/--head are incompatible with subset float32 mode");
        }
        if (!opts.group_selector.empty()) {
            throw std::runtime_error("--group is incompatible with subset float32 mode");
        }
        if (!opts.output_path.empty()) {
            throw std::runtime_error("Subset float32 mode writes binary data to stdout; do not pass output path");
        }
        if (opts.stream_id < 0) {
            throw std::runtime_error("--stream-id is required for subset float32 mode");
        }
    }
    if (!(opts.zarr_compress == "none" || opts.zarr_compress == "gzip")) {
        throw std::runtime_error("--zarr-compress supports only: none or gzip");
    }
}

std::vector<std::string> split_tab(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

double decode_transform(double value, bool log, bool asinh, double offset,
                        bool shifted=false, double scale=1, double origin=0, double span=1) {
    return avenc_transform::inverse(value,log,asinh,offset,shifted,scale,origin,span);
}

void set_key_from_text(codes_handle *h, const char *key, const std::string &value) {
    if (!h || !key || value.empty()) return;
    if (is_integer_token(value)) {
        try {
            long v = std::stol(value);
            if (codes_set_long(h, key, v) == 0) return;
        } catch (...) {
            // Fall through to string set.
        }
    }
    size_t len = value.size();
    (void)codes_set_string(h, key, value.c_str(), &len);
}

void set_validity_from_timestamp(codes_handle *h, const std::string &timestamp) {
    avenc_grib_time::restore(h, timestamp);
}

std::string tsv_unescape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '\\' && i + 1 < in.size()) {
            char n = in[i + 1];
            if (n == 't') {
                out.push_back('\t');
                ++i;
                continue;
            }
            if (n == 'n') {
                out.push_back('\n');
                ++i;
                continue;
            }
            if (n == 'r') {
                out.push_back('\r');
                ++i;
                continue;
            }
            if (n == '\\') {
                out.push_back('\\');
                ++i;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::string tsv_escape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

std::string ffmetadata_escape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        if (c == '\\' || c == ';' || c == '#' || c == '=') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string json_escape(const std::string &in) {
    std::ostringstream oss;
    for (unsigned char c : in) {
        switch (c) {
        case '\"': oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\b': oss << "\\b"; break;
        case '\f': oss << "\\f"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
            if (c < 0x20) {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c)
                    << std::dec << std::setfill(' ');
            } else {
                oss << static_cast<char>(c);
            }
        }
    }
    return oss.str();
}

std::string read_command_stdout(const std::vector<std::string> &args) {
    std::ostringstream cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd << ' ';
        cmd << '"' << args[i] << '"';
    }
    FILE *pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) throw std::runtime_error("popen failed");

    std::string out;
    char buf[4096];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), pipe);
        if (n == 0) break;
        out.append(buf, n);
    }
    int ret = pclose(pipe);
    if (ret != 0) throw std::runtime_error("Command failed: " + cmd.str());
    return out;
}

int run_command(const std::vector<std::string> &args, bool verbose) {
    if (args.empty()) return 0;
    if (verbose) {
        std::cerr << "[cmd]";
        for (const auto &a : args) std::cerr << " " << a;
        std::cerr << "\n";
    }
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) throw std::runtime_error("waitpid failed");
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

std::string make_temp_path(const std::string &prefix, const std::string &suffix) {
    std::string templ = "/tmp/" + prefix + "XXXXXX" + suffix;
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), static_cast<int>(suffix.size()));
    if (fd < 0) {
        throw std::runtime_error("mkstemps failed for " + templ + ": " + std::strerror(errno));
    }
    close(fd);
    return std::string(buf.data());
}

void write_text_file(const std::string &path, const std::string &content) {
    std::ofstream f(path);
    if (!f.is_open()) throw std::runtime_error("Unable to write file: " + path);
    f << content;
    if (!f.good()) throw std::runtime_error("Failed writing file: " + path);
}

std::vector<uint8_t> base64_decode(const std::string &in) {
    static int table[256];
    static bool init = false;
    if (!init) {
        std::fill(std::begin(table), std::end(table), -1);
        const std::string alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t i = 0; i < alpha.size(); ++i) {
            table[static_cast<unsigned char>(alpha[i])] = static_cast<int>(i);
        }
        init = true;
    }

    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4);
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = table[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string base64_encode(const std::vector<uint8_t> &in) {
    static const char *alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                     (static_cast<uint32_t>(in[i + 1]) << 8) |
                     static_cast<uint32_t>(in[i + 2]);
        out.push_back(alpha[(v >> 18) & 0x3F]);
        out.push_back(alpha[(v >> 12) & 0x3F]);
        out.push_back(alpha[(v >> 6) & 0x3F]);
        out.push_back(alpha[v & 0x3F]);
    }
    if (i < in.size()) {
        uint32_t v = static_cast<uint32_t>(in[i]) << 16;
        out.push_back(alpha[(v >> 18) & 0x3F]);
        if (i + 1 < in.size()) {
            v |= static_cast<uint32_t>(in[i + 1]) << 8;
            out.push_back(alpha[(v >> 12) & 0x3F]);
            out.push_back(alpha[(v >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back(alpha[(v >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

std::vector<uint8_t> zlib_decompress(const std::vector<uint8_t> &in) {
    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) throw std::runtime_error("inflateInit failed");

    std::vector<uint8_t> out;
    out.reserve(in.size() * 2 + 1024);
    std::vector<uint8_t> chunk(1 << 14);

    strm.next_in = const_cast<Bytef *>(in.data());
    strm.avail_in = static_cast<uInt>(in.size());

    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            throw std::runtime_error("inflate failed");
        }
        size_t have = chunk.size() - strm.avail_out;
        out.insert(out.end(), chunk.data(), chunk.data() + have);
    }

    inflateEnd(&strm);
    return out;
}

std::vector<uint8_t> zlib_compress(const std::string &text, int level = Z_BEST_COMPRESSION) {
    z_stream strm{};
    if (deflateInit(&strm, level) != Z_OK) throw std::runtime_error("deflateInit failed");

    std::vector<uint8_t> out;
    out.reserve(text.size() / 2 + 1024);
    std::vector<uint8_t> chunk(1 << 14);

    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));
    strm.avail_in = static_cast<uInt>(text.size());

    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        ret = deflate(&strm, strm.avail_in ? Z_NO_FLUSH : Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            deflateEnd(&strm);
            throw std::runtime_error("deflate failed");
        }
        size_t have = chunk.size() - strm.avail_out;
        out.insert(out.end(), chunk.data(), chunk.data() + have);
    }

    deflateEnd(&strm);
    return out;
}

std::vector<uint8_t> gzip_compress_bytes(const uint8_t *data, size_t n, int level = 6) {
    z_stream strm{};
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2(gzip) failed");
    }
    strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
    strm.avail_in = static_cast<uInt>(n);
    std::vector<uint8_t> out;
    std::vector<uint8_t> chunk(1 << 14);
    int rc = Z_OK;
    do {
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        rc = deflate(&strm, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            deflateEnd(&strm);
            throw std::runtime_error("gzip deflate failed");
        }
        const size_t have = chunk.size() - strm.avail_out;
        out.insert(out.end(), chunk.data(), chunk.data() + have);
    } while (rc != Z_STREAM_END);
    deflateEnd(&strm);
    return out;
}

void write_bytes_file(const std::string &path, const uint8_t *data, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Unable to write file: " + path);
    if (n > 0) f.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(n));
    if (!f.good()) throw std::runtime_error("Write failed: " + path);
}

int64_t parse_timestamp_ms(const std::string &ts) {
    // YYYY-MM-DDTHH:MM:SSZ
    if (ts.size() < 20) return -1;
    if (ts[4] != '-' || ts[7] != '-' || ts[10] != 'T' || ts[13] != ':' || ts[16] != ':') return -1;
    std::tm t{};
    try {
        t.tm_year = std::stoi(ts.substr(0, 4)) - 1900;
        t.tm_mon = std::stoi(ts.substr(5, 2)) - 1;
        t.tm_mday = std::stoi(ts.substr(8, 2));
        t.tm_hour = std::stoi(ts.substr(11, 2));
        t.tm_min = std::stoi(ts.substr(14, 2));
        t.tm_sec = std::stoi(ts.substr(17, 2));
    } catch (...) {
        return -1;
    }
#if defined(_WIN32)
    const std::time_t epoch = _mkgmtime(&t);
#else
    const std::time_t epoch = timegm(&t);
#endif
    if (epoch < 0) return -1;
    return static_cast<int64_t>(epoch) * 1000LL;
}

std::string sanitize_token(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) out = "stream";
    return out;
}

std::string extract_comment(const std::string &video) {
    std::vector<std::string> cmd = {
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "format_tags=comment:stream_tags=comment",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        video,
    };
    std::string out = read_command_stdout(cmd);
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (t.rfind("AVENCv1|", 0) == 0 || t.rfind("AVENCv2|", 0) == 0) return t;
    }
    throw std::runtime_error("No AVENC metadata comment found in MP4");
}

std::string extract_json_string_field(const std::string &json, const std::string &key) {
    std::string pat = "\"" + key + "\":\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) throw std::runtime_error("Missing metadata field: " + key);
    p += pat.size();
    size_t e = p;
    while (e < json.size()) {
        if (json[e] == '"' && json[e - 1] != '\\') break;
        ++e;
    }
    if (e >= json.size()) throw std::runtime_error("Unterminated JSON string field: " + key);
    return json.substr(p, e - p);
}

uint32_t read_u32le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64le(const uint8_t *p) {
    return static_cast<uint64_t>(p[0]) |
           (static_cast<uint64_t>(p[1]) << 8) |
           (static_cast<uint64_t>(p[2]) << 16) |
           (static_cast<uint64_t>(p[3]) << 24) |
           (static_cast<uint64_t>(p[4]) << 32) |
           (static_cast<uint64_t>(p[5]) << 40) |
           (static_cast<uint64_t>(p[6]) << 48) |
           (static_cast<uint64_t>(p[7]) << 56);
}

int32_t read_i32le(const uint8_t *p) {
    return static_cast<int32_t>(read_u32le(p));
}

double read_f64le(const uint8_t *p) {
    uint64_t bits = read_u64le(p);
    double v = 0.0;
    static_assert(sizeof(double) == sizeof(uint64_t), "Unexpected double size");
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

uint32_t read_u32le_at(const std::vector<uint8_t> &bytes, size_t &off, const char *what) {
    if (off + 4 > bytes.size()) throw std::runtime_error(std::string("Truncated AVENCv2 field: ") + what);
    uint32_t v = read_u32le(bytes.data() + off);
    off += 4;
    return v;
}

uint64_t read_u64le_at(const std::vector<uint8_t> &bytes, size_t &off, const char *what) {
    if (off + 8 > bytes.size()) throw std::runtime_error(std::string("Truncated AVENCv2 field: ") + what);
    uint64_t v = read_u64le(bytes.data() + off);
    off += 8;
    return v;
}

int32_t read_i32le_at(const std::vector<uint8_t> &bytes, size_t &off, const char *what) {
    return static_cast<int32_t>(read_u32le_at(bytes, off, what));
}

double read_f64le_at(const std::vector<uint8_t> &bytes, size_t &off, const char *what) {
    if (off + 8 > bytes.size()) throw std::runtime_error(std::string("Truncated AVENCv2 field: ") + what);
    double v = read_f64le(bytes.data() + off);
    off += 8;
    return v;
}

std::string read_string_u32le_at(const std::vector<uint8_t> &bytes, size_t &off, const char *what) {
    const uint32_t n = read_u32le_at(bytes, off, what);
    if (off + static_cast<size_t>(n) > bytes.size()) {
        throw std::runtime_error(std::string("Truncated AVENCv2 string: ") + what);
    }
    std::string out(reinterpret_cast<const char *>(bytes.data() + off), n);
    off += static_cast<size_t>(n);
    return out;
}

void append_u32le(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void append_u64le(std::vector<uint8_t> &out, uint64_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 32) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 40) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 48) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 56) & 0xFFu));
}

void append_i32le(std::vector<uint8_t> &out, int32_t v) {
    append_u32le(out, static_cast<uint32_t>(v));
}

void append_f64le(std::vector<uint8_t> &out, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    append_u64le(out, bits);
}

void append_string_u32le(std::vector<uint8_t> &out, const std::string &s) {
    if (s.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error("String too long for AVENCv2 encoding");
    }
    append_u32le(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

struct DecodedComment {
    int version = 1; // 1 or 2
    bool has_binary_recon = false;
    ReconPayload recon_binary;
    std::string metadata_json;
    std::string recon_format;
    std::string recon_payload;
    size_t comment_chars = 0;
    size_t payload_base64_chars = 0;
    size_t payload_compressed_bytes = 0;
    size_t payload_uncompressed_bytes = 0;
};

ReconPayload parse_recon_payload_v2_binary(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 36) throw std::runtime_error("AVENCv2 payload too short");
    const char expect_magic[8] = {'A', 'V', '2', 'M', 'E', 'T', 'A', '\0'};
    if (std::memcmp(bytes.data(), expect_magic, 8) != 0) {
        throw std::runtime_error("Invalid AVENCv2 metadata magic");
    }
    const uint32_t fmt_ver = read_u32le(bytes.data() + 8);
    if ((fmt_ver != 2u && fmt_ver != 3u && fmt_ver != 4u)) {
        throw std::runtime_error("Unsupported AVENCv2 format version (expected 2, 3 or 4)");
    }
    size_t off = 12;
    (void)read_u32le_at(bytes, off, "flags");

    ReconPayload out;
    out.nx = static_cast<int>(read_u32le_at(bytes, off, "nx"));
    out.ny = static_cast<int>(read_u32le_at(bytes, off, "ny"));
    const uint32_t stream_count = read_u32le_at(bytes, off, "stream_count");
    const uint32_t record_count = read_u32le_at(bytes, off, "record_count");
    (void)read_u32le_at(bytes, off, "reserved");

    out.streams.reserve(stream_count);
    for (uint32_t i = 0; i < stream_count; ++i) {
        StreamInfo s;
        s.stream_index = read_i32le_at(bytes, off, "stream_index");
        s.frame_count = static_cast<size_t>(read_u32le_at(bytes, off, "stream.frame_count"));
        s.bits = static_cast<int>(read_u32le_at(bytes, off, "stream.bits"));
        s.storage_shift = read_i32le_at(bytes, off, "stream.storage_shift");
        s.min_value = read_f64le_at(bytes, off, "stream.min_value");
        s.max_value = read_f64le_at(bytes, off, "stream.max_value");
        const uint32_t transform = read_u32le_at(bytes, off, "stream.transform");
        if (transform > (fmt_ver == 4u ? 4u : fmt_ver == 3u ? 2u : 1u))
            throw std::runtime_error("Unsupported stream transform");
        s.apply_log2 = transform == 1u;
        s.apply_asinh = transform == 2u || transform == 3u;
        s.apply_shifted_log=transform==4u;
        s.log_offset = read_f64le_at(bytes, off, "stream.log_offset");
        if(fmt_ver>=4u){
            s.transform_scale=read_f64le_at(bytes,off,"stream.transform_scale");
            s.transform_origin=read_f64le_at(bytes,off,"stream.transform_origin");
            s.transform_span=read_f64le_at(bytes,off,"stream.transform_span");
            avenc_transform::validate(s.transform_scale,s.transform_origin,s.transform_span);
        }
        s.encoded_nx = static_cast<int>(read_u32le_at(bytes, off, "stream.encoded_nx"));
        s.encoded_ny = static_cast<int>(read_u32le_at(bytes, off, "stream.encoded_ny"));
        s.crop_nx = static_cast<int>(read_u32le_at(bytes, off, "stream.crop_nx"));
        s.crop_ny = static_cast<int>(read_u32le_at(bytes, off, "stream.crop_ny"));
        const std::string bitmap_payload = read_string_u32le_at(bytes, off, "stream.bitmap_payload");
        s.template_b64 = read_string_u32le_at(bytes, off, "stream.template_b64");
        if (!bitmap_payload.empty()) {
            auto bmp_zip = base64_decode(bitmap_payload);
            s.shared_bitmap = zlib_decompress(bmp_zip);
        }
        out.streams.push_back(std::move(s));
    }

    out.records.reserve(record_count);
    for (uint32_t i = 0; i < record_count; ++i) {
        RecordInfo r;
        r.order = static_cast<size_t>(read_u64le_at(bytes, off, "record.order"));
        r.stream_index = read_i32le_at(bytes, off, "record.stream_index");
        r.frame_index = static_cast<size_t>(read_u32le_at(bytes, off, "record.frame_index"));
        r.file_index = read_i32le_at(bytes, off, "record.file_index");
        r.message_index = static_cast<size_t>(read_u64le_at(bytes, off, "record.message_index"));
        r.frame_min = read_f64le_at(bytes, off, "record.frame_min");
        r.frame_max = read_f64le_at(bytes, off, "record.frame_max");
        r.bitmap_present = (read_u32le_at(bytes, off, "record.bitmap_present") != 0u);
        r.bits_per_value = read_i32le_at(bytes, off, "record.bits_per_value");
        r.binary_scale_shift = read_i32le_at(bytes, off, "record.binary_scale_shift");
        r.short_name = read_string_u32le_at(bytes, off, "record.short_name");
        r.type_of_level = read_string_u32le_at(bytes, off, "record.type_of_level");
        r.level = read_string_u32le_at(bytes, off, "record.level");
        r.data_date = read_string_u32le_at(bytes, off, "record.data_date");
        r.data_time = read_string_u32le_at(bytes, off, "record.data_time");
        r.step_range = read_string_u32le_at(bytes, off, "record.step_range");
        r.timestamp = read_string_u32le_at(bytes, off, "record.timestamp");
        r.ensemble_number = read_string_u32le_at(bytes, off, "record.ensemble_number");
        r.perturbation_number = read_string_u32le_at(bytes, off, "record.perturbation_number");
        r.template_b64 = read_string_u32le_at(bytes, off, "record.template_b64");
        out.records.push_back(std::move(r));
    }

    if (out.nx <= 0 || out.ny <= 0) throw std::runtime_error("Invalid AVENCv2 GRID");
    if (out.streams.empty()) throw std::runtime_error("No STREAM rows in AVENCv2 metadata");
    if (out.records.empty()) throw std::runtime_error("No RECORD rows in AVENCv2 metadata");
    for (auto &s : out.streams) {
        if (s.encoded_nx <= 0) s.encoded_nx = out.nx;
        if (s.encoded_ny <= 0) s.encoded_ny = out.ny;
        if (s.crop_nx <= 0) s.crop_nx = out.nx;
        if (s.crop_ny <= 0) s.crop_ny = out.ny;
    }
    return out;
}

DecodedComment decode_avenc_comment(const std::string &comment) {
    DecodedComment out;
    out.comment_chars = comment.size();
    if (comment.rfind("AVENCv1|", 0) == 0) {
        out.version = 1;
        const std::string meta_b64 = comment.substr(std::strlen("AVENCv1|"));
        out.payload_base64_chars = meta_b64.size();
        const auto meta_zip = base64_decode(meta_b64);
        out.payload_compressed_bytes = meta_zip.size();
        const auto meta_json_bytes = zlib_decompress(meta_zip);
        out.payload_uncompressed_bytes = meta_json_bytes.size();
        out.metadata_json.assign(meta_json_bytes.begin(), meta_json_bytes.end());
        out.recon_format = extract_json_string_field(out.metadata_json, "reconstruct_format");
        out.recon_payload = extract_json_string_field(out.metadata_json, "reconstruct_payload");
        return out;
    }

    if (comment.rfind("AVENCv2|", 0) == 0) {
        out.version = 2;
        const std::string meta_b64 = comment.substr(std::strlen("AVENCv2|"));
        out.payload_base64_chars = meta_b64.size();
        const auto meta_zip = base64_decode(meta_b64);
        out.payload_compressed_bytes = meta_zip.size();
        const auto bytes = zlib_decompress(meta_zip);
        out.payload_uncompressed_bytes = bytes.size();
        out.has_binary_recon = true;
        out.recon_binary = parse_recon_payload_v2_binary(bytes);
        return out;
    }

    throw std::runtime_error("Unsupported AVENC metadata comment prefix");
}

ReconPayload parse_recon_payload(const std::string &text) {
    ReconPayload out;
    std::istringstream iss(text);
    std::string line;

    if (!std::getline(iss, line)) throw std::runtime_error("Empty reconstruction payload");
    if (trim(line) != "AVPACK_RECON_V1") {
        throw std::runtime_error("Unsupported reconstruction payload header");
    }

    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;
        auto cols = split_tab(line);
        if (cols.empty()) continue;

        if (cols[0] == "GRID") {
            if (cols.size() < 3) throw std::runtime_error("Invalid GRID row");
            out.nx = std::stoi(cols[1]);
            out.ny = std::stoi(cols[2]);
            continue;
        }

        if (cols[0] == "STREAM") {
            if (cols.size() < 6) throw std::runtime_error("Invalid STREAM row");
            StreamInfo s;
            s.stream_index = std::stoi(cols[1]);
            s.frame_count = static_cast<size_t>(std::stoull(cols[2]));
            s.bits = std::stoi(cols[3]);
            s.min_value = std::stod(cols[4]);
            s.max_value = std::stod(cols[5]);
            if (cols.size() >= 7) s.storage_shift = std::stoi(cols[6]);
            if(cols.size()>=18){s.transform_scale=std::stod(cols[15]);s.transform_origin=std::stod(cols[16]);s.transform_span=std::stod(cols[17]);avenc_transform::validate(s.transform_scale,s.transform_origin,s.transform_span);}
            if (cols.size() >= 8) {
                const std::string mode = trim(cols[7]);
                s.apply_log2 = (mode == "log2p1" || mode == "log2");
                s.apply_asinh = (mode == "asinh");
                s.apply_shifted_log=(mode=="shifted-log");
            }
            if (cols.size() >= 9) {
                try {
                    s.log_offset = std::stod(cols[8]);
                } catch (...) {
                    s.log_offset = 1.0;
                }
                if (!std::isfinite(s.log_offset) || s.log_offset <= 0.0) s.log_offset = 1.0;
            }
            if (cols.size() >= 10) {
                const std::string bitmap_payload = trim(cols[9]);
                if (!bitmap_payload.empty() && bitmap_payload != "-") {
                    auto bmp_zip = base64_decode(bitmap_payload);
                    s.shared_bitmap = zlib_decompress(bmp_zip);
                }
            }
            if (cols.size() >= 11) {
                s.template_b64 = cols[10];
            }
            if (cols.size() >= 13) {
                try {
                    s.encoded_nx = std::stoi(cols[11]);
                    s.encoded_ny = std::stoi(cols[12]);
                } catch (...) {
                    s.encoded_nx = 0;
                    s.encoded_ny = 0;
                }
            }
            if (cols.size() >= 15) {
                try {
                    s.crop_nx = std::stoi(cols[13]);
                    s.crop_ny = std::stoi(cols[14]);
                } catch (...) {
                    s.crop_nx = 0;
                    s.crop_ny = 0;
                }
            }
            out.streams.push_back(s);
            continue;
        }

        if (cols[0] == "RECORD") {
            if (cols.size() < 19) throw std::runtime_error("Invalid RECORD row");
            RecordInfo r;
            r.order = static_cast<size_t>(std::stoull(cols[1]));
            r.stream_index = std::stoi(cols[2]);
            r.frame_index = static_cast<size_t>(std::stoull(cols[3]));
            r.file_index = std::stoi(cols[4]);
            r.message_index = static_cast<size_t>(std::stoull(cols[5]));
            r.short_name = tsv_unescape(cols[6]);
            r.type_of_level = tsv_unescape(cols[7]);
            r.level = tsv_unescape(cols[8]);
            r.data_date = tsv_unescape(cols[9]);
            r.data_time = tsv_unescape(cols[10]);
            r.step_range = tsv_unescape(cols[11]);
            r.timestamp = tsv_unescape(cols[12]);
            r.ensemble_number = tsv_unescape(cols[13]);
            r.perturbation_number = tsv_unescape(cols[14]);
            r.frame_min = std::stod(cols[15]);
            r.frame_max = std::stod(cols[16]);
            r.bitmap_present = (cols[17] == "1");
            r.template_b64 = cols[18];
            if (cols.size() >= 20) {
                try {
                    r.bits_per_value = std::stoi(cols[19]);
                } catch (...) {
                    r.bits_per_value = -1;
                }
            }
            if (cols.size() >= 23) {
                try {
                    r.binary_scale_shift = std::stoi(cols[22]);
                } catch (...) {
                    r.binary_scale_shift = 0;
                }
            }
            out.records.push_back(std::move(r));
            continue;
        }
    }

    if (out.nx <= 0 || out.ny <= 0) throw std::runtime_error("Missing GRID info in reconstruction payload");
    if (out.streams.empty()) throw std::runtime_error("No STREAM rows in reconstruction payload");
    if (out.records.empty()) throw std::runtime_error("No RECORD rows in reconstruction payload");
    for (auto &s : out.streams) {
        if (s.encoded_nx <= 0) s.encoded_nx = out.nx;
        if (s.encoded_ny <= 0) s.encoded_ny = out.ny;
        if (s.crop_nx <= 0) s.crop_nx = out.nx;
        if (s.crop_ny <= 0) s.crop_ny = out.ny;
    }
    return out;
}

std::unordered_map<int, const StreamInfo *> build_stream_lookup(const ReconPayload &recon) {
    std::unordered_map<int, const StreamInfo *> lookup;
    lookup.reserve(recon.streams.size());
    for (const auto &s : recon.streams) lookup[s.stream_index] = &s;
    return lookup;
}

std::vector<GroupInfo> build_groups(const ReconPayload &recon) {
    std::unordered_map<int, std::vector<const RecordInfo *>> by_stream;
    for (const auto &r : recon.records) by_stream[r.stream_index].push_back(&r);

    std::vector<GroupInfo> groups;
    groups.reserve(recon.streams.size());
    for (const auto &s : recon.streams) {
        auto it = by_stream.find(s.stream_index);
        if (it == by_stream.end() || it->second.empty()) continue;

        GroupInfo g;
        g.stream_index = s.stream_index;
        g.frame_count = s.frame_count;
        g.record_count = it->second.size();
        g.bits = s.bits;
        g.storage_shift = s.storage_shift;
        g.min_value = s.min_value;
        g.max_value = s.max_value;

        std::map<std::string, size_t> short_counts;
        std::set<std::string> levels;
        std::set<std::string> steps;
        for (const auto *rp : it->second) {
            short_counts[rp->short_name]++;
            levels.insert(rp->level);
            steps.insert(rp->data_date + "/" + rp->data_time + "/" + rp->step_range);
            if (g.type_of_level.empty() && !rp->type_of_level.empty()) g.type_of_level = rp->type_of_level;
        }
        g.unique_levels = levels.size();
        g.unique_steps = steps.size();

        if (short_counts.empty()) g.alias = "stream_" + std::to_string(s.stream_index);
        else if (short_counts.size() == 1) g.alias = short_counts.begin()->first;
        else {
            auto best = std::max_element(short_counts.begin(), short_counts.end(),
                                         [](const auto &a, const auto &b) {
                                             return a.second < b.second;
                                         });
            g.alias = best->first;
        }
        g.short_name = g.alias;
        groups.push_back(std::move(g));
    }

    std::map<std::string, int> alias_count;
    for (const auto &g : groups) alias_count[g.alias]++;
    for (auto &g : groups) {
        if (alias_count[g.alias] > 1) g.display_name = g.alias + "@s" + std::to_string(g.stream_index);
        else g.display_name = g.alias;
    }

    std::sort(groups.begin(), groups.end(), [](const GroupInfo &a, const GroupInfo &b) {
        return a.stream_index < b.stream_index;
    });
    return groups;
}

std::set<int> resolve_selected_streams(const ReconPayload &recon,
                                       const std::vector<GroupInfo> &groups,
                                       const std::string &selector_csv) {
    std::set<int> selected;
    if (trim(selector_csv).empty()) {
        for (const auto &s : recon.streams) selected.insert(s.stream_index);
        return selected;
    }

    const auto tokens = split_csv(selector_csv);
    if (tokens.empty()) throw std::runtime_error("Empty --group selector");

    for (const auto &token : tokens) {
        bool matched = false;
        if (token.rfind("stream:", 0) == 0) {
            const std::string idx_txt = token.substr(std::strlen("stream:"));
            if (!is_integer_token(idx_txt)) {
                throw std::runtime_error("Invalid stream selector: " + token);
            }
            selected.insert(std::stoi(idx_txt));
            matched = true;
        } else if (is_integer_token(token)) {
            selected.insert(std::stoi(token));
            matched = true;
        }

        for (const auto &g : groups) {
            if (token == g.alias || token == g.display_name) {
                selected.insert(g.stream_index);
                matched = true;
            }
        }
        if (!matched) {
            throw std::runtime_error("Unknown group selector token: " + token);
        }
    }

    std::set<int> known;
    for (const auto &s : recon.streams) known.insert(s.stream_index);
    for (int idx : selected) {
        if (known.count(idx) == 0) {
            throw std::runtime_error("Selected stream index not present in input MP4: " +
                                     std::to_string(idx));
        }
    }
    if (selected.empty()) throw std::runtime_error("No streams selected");
    return selected;
}

void print_groups(const ReconPayload &recon, const std::vector<GroupInfo> &groups) {
    std::cout << "grid\t" << recon.nx << "x" << recon.ny << "\n";
    std::unordered_map<int, const StreamInfo *> stream_by_idx;
    for (const auto &s : recon.streams) stream_by_idx[s.stream_index] = &s;
    std::cout << "group\tstream\tframes\trecords\tshortName\ttypeOfLevel\tlevels\tsteps\tbits\tstorageShift\tmin\tmax\tlog2\tsharedBitmap\ttransform\n";
    for (const auto &g : groups) {
        const StreamInfo *s = nullptr;
        auto it = stream_by_idx.find(g.stream_index);
        if (it != stream_by_idx.end()) s = it->second;
        std::cout << g.display_name << "\t" << g.stream_index << "\t" << g.frame_count << "\t"
                  << g.record_count << "\t" << g.short_name << "\t" << g.type_of_level << "\t"
                  << g.unique_levels << "\t" << g.unique_steps << "\t" << g.bits << "\t"
                  << g.storage_shift << "\t" << std::setprecision(10) << g.min_value << "\t"
                  << std::setprecision(10) << g.max_value << "\t"
                  << ((s && s->apply_log2) ? 1 : 0) << "\t"
                  << ((s && !s->shared_bitmap.empty()) ? 1 : 0) << "\t"
                  << ((s && s->apply_shifted_log)?"shifted-log":(s && s->apply_asinh) ? "asinh" : (s && s->apply_log2) ? "log" : "linear") << "\n";
    }
}

void print_group_records(const ReconPayload &recon, const std::set<int> &selected_streams) {
    std::vector<RecordInfo> rows;
    rows.reserve(recon.records.size());
    for (const auto &r : recon.records) {
        if (selected_streams.count(r.stream_index)) rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const RecordInfo &a, const RecordInfo &b) {
        return a.order < b.order;
    });

    std::cout << "order\tstream\tframe\tshortName\ttypeOfLevel\tlevel\tdataDate\tdataTime\tstepRange\tens\tpert\tframeMin\tframeMax\tbitmap\n";
    for (const auto &r : rows) {
        std::cout << r.order << "\t" << r.stream_index << "\t" << r.frame_index << "\t"
                  << r.short_name << "\t" << r.type_of_level << "\t" << r.level << "\t"
                  << r.data_date << "\t" << r.data_time << "\t" << r.step_range << "\t"
                  << r.ensemble_number << "\t" << r.perturbation_number << "\t"
                  << std::setprecision(10) << r.frame_min << "\t" << std::setprecision(10)
                  << r.frame_max << "\t" << (r.bitmap_present ? 1 : 0) << "\n";
    }
}

size_t remote_object_size = 0;
size_t http_grib_messages_written = 0;

size_t file_size_bytes(const std::string &path) {
    if (AvencHttpInput::is_url(path) && remote_object_size) return remote_object_size;
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        throw std::runtime_error("Unable to stat file: " + path);
    }
    return static_cast<size_t>(st.st_size);
}

size_t current_rss_bytes() {
#ifdef __linux__
    std::ifstream f("/proc/self/statm");
    long pages_total = 0;
    long pages_rss = 0;
    if (f.is_open() && (f >> pages_total >> pages_rss)) {
        const long ps = sysconf(_SC_PAGESIZE);
        if (ps > 0 && pages_rss >= 0) {
            return static_cast<size_t>(pages_rss) * static_cast<size_t>(ps);
        }
    }
#endif
    struct rusage ru {};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#ifdef __APPLE__
    return static_cast<size_t>(ru.ru_maxrss);
#else
    return static_cast<size_t>(ru.ru_maxrss) * 1024ULL;
#endif
}

size_t peak_rss_bytes() {
    struct rusage ru {};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#ifdef __APPLE__
    return static_cast<size_t>(ru.ru_maxrss);
#else
    return static_cast<size_t>(ru.ru_maxrss) * 1024ULL;
#endif
}

double bytes_to_mib(size_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

std::unordered_map<int, uint64_t> probe_stream_packet_sizes(const std::string &video) {
    std::vector<std::string> cmd = {
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v",
        "-show_entries",
        "packet=stream_index,size",
        "-of",
        "csv=p=0",
        video,
    };
    std::unordered_map<int, uint64_t> out;
    std::string text = read_command_stdout(cmd);
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;
        auto cols = split_csv(line);
        if (cols.size() < 2) continue;
        if (!is_integer_token(cols[0]) || !is_integer_token(cols[1])) continue;
        const int stream_idx = std::stoi(cols[0]);
        const long long size = std::stoll(cols[1]);
        if (size > 0) out[stream_idx] += static_cast<uint64_t>(size);
    }
    return out;
}

void print_metadata_head(const DecodedComment &decoded, const ReconPayload &recon) {
    if (decoded.version == 1) {
        std::cout << decoded.metadata_json << "\n";
        return;
    }
    std::cout << "# AVENCv2 (binary fmt=" << (std::any_of(recon.streams.begin(), recon.streams.end(),
        [](const StreamInfo &s) { return s.apply_shifted_log||(s.apply_asinh&&s.transform_scale!=1); }) ? 4 : std::any_of(recon.streams.begin(),recon.streams.end(),[](const StreamInfo& s){return s.apply_asinh;})?3:2) << ")\n";
    std::cout << serialize_recon_payload(recon);
}

void print_stats(const std::string &input_mp4,
                 const DecodedComment &decoded,
                 const ReconPayload &recon,
                 double parse_ms,
                 size_t rss_before,
                 size_t rss_after,
                 size_t peak_before,
                 size_t peak_after) {
    const size_t file_bytes = file_size_bytes(input_mp4);
    const auto packet_sizes = probe_stream_packet_sizes(input_mp4);

    uint64_t total_stream_packet_bytes = 0;
    for (const auto &kv : packet_sizes) total_stream_packet_bytes += kv.second;

    uint64_t total_frames = 0;
    uint64_t total_raw32_equiv = 0;
    for (const auto &s : recon.streams) {
        const int nx = (s.crop_nx > 0) ? s.crop_nx : recon.nx;
        const int ny = (s.crop_ny > 0) ? s.crop_ny : recon.ny;
        const uint64_t raw = static_cast<uint64_t>(s.frame_count) *
                             static_cast<uint64_t>(std::max(0, nx)) *
                             static_cast<uint64_t>(std::max(0, ny)) * 4ULL;
        total_raw32_equiv += raw;
        total_frames += static_cast<uint64_t>(s.frame_count);
    }

    const double raw_to_file_ratio = (file_bytes > 0)
        ? static_cast<double>(total_raw32_equiv) / static_cast<double>(file_bytes)
        : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "file\t" << input_mp4 << "\n";
    std::cout << "metadata_version\t" << decoded.version << "\n";
    std::cout << "streams\t" << recon.streams.size() << "\n";
    std::cout << "records\t" << recon.records.size() << "\n";
    std::cout << "grid\t" << recon.nx << "x" << recon.ny << "\n";
    std::cout << "total_frames\t" << total_frames << "\n";
    std::cout << "file_size_bytes\t" << file_bytes << "\n";
    std::cout << "stream_payload_bytes_total\t" << total_stream_packet_bytes << "\n";
    std::cout << "metadata_comment_chars\t" << decoded.comment_chars << "\n";
    std::cout << "metadata_b64_chars\t" << decoded.payload_base64_chars << "\n";
    std::cout << "metadata_header_compressed_bytes\t" << decoded.payload_compressed_bytes << "\n";
    std::cout << "metadata_header_uncompressed_bytes\t" << decoded.payload_uncompressed_bytes << "\n";
    std::cout << "raw32_equivalent_total_bytes\t" << total_raw32_equiv << "\n";
    std::cout << "compression_ratio_raw32_to_mp4\t" << raw_to_file_ratio << "\n";
    std::cout << "metadata_parse_time_ms\t" << parse_ms << "\n";
    std::cout << "metadata_parse_rss_before_mib\t" << bytes_to_mib(rss_before) << "\n";
    std::cout << "metadata_parse_rss_after_mib\t" << bytes_to_mib(rss_after) << "\n";
    std::cout << "metadata_parse_rss_delta_mib\t" << bytes_to_mib(
        (rss_after > rss_before) ? (rss_after - rss_before) : 0ULL) << "\n";
    std::cout << "metadata_parse_peak_rss_before_mib\t" << bytes_to_mib(peak_before) << "\n";
    std::cout << "metadata_parse_peak_rss_after_mib\t" << bytes_to_mib(peak_after) << "\n";
    std::cout << "metadata_parse_peak_rss_delta_mib\t" << bytes_to_mib(
        (peak_after > peak_before) ? (peak_after - peak_before) : 0ULL) << "\n";

    std::cout << "\n";
    std::cout << "stream\tframes\tcrop\tbits\tpacket_bytes\traw32_equiv_bytes\traw32_to_stream_ratio\n";
    for (const auto &s : recon.streams) {
        const int nx = (s.crop_nx > 0) ? s.crop_nx : recon.nx;
        const int ny = (s.crop_ny > 0) ? s.crop_ny : recon.ny;
        const uint64_t raw = static_cast<uint64_t>(s.frame_count) *
                             static_cast<uint64_t>(std::max(0, nx)) *
                             static_cast<uint64_t>(std::max(0, ny)) * 4ULL;
        const uint64_t packet = (packet_sizes.count(s.stream_index) > 0)
            ? packet_sizes.at(s.stream_index) : 0ULL;
        const double ratio = (packet > 0)
            ? static_cast<double>(raw) / static_cast<double>(packet)
            : 0.0;
        std::cout << s.stream_index << "\t" << s.frame_count << "\t"
                  << nx << "x" << ny << "\t" << s.bits << "\t"
                  << packet << "\t" << raw << "\t" << ratio << "\n";
    }
}

ReconPayload filter_recon(const ReconPayload &recon,
                          const std::set<int> &selected_streams,
                          bool remap_stream_indices) {
    ReconPayload out;
    out.nx = recon.nx;
    out.ny = recon.ny;

    std::unordered_map<int, int> remap;
    int next_idx = 0;
    for (const auto &s : recon.streams) {
        if (selected_streams.count(s.stream_index) == 0) continue;
        StreamInfo copy = s;
        if (remap_stream_indices) {
            remap[s.stream_index] = next_idx;
            copy.stream_index = next_idx++;
        }
        out.streams.push_back(copy);
    }
    if (out.streams.empty()) throw std::runtime_error("No STREAM selected");

    std::vector<RecordInfo> records = recon.records;
    std::sort(records.begin(), records.end(), [](const RecordInfo &a, const RecordInfo &b) {
        return a.order < b.order;
    });

    size_t next_order = 0;
    for (auto r : records) {
        if (selected_streams.count(r.stream_index) == 0) continue;
        if (remap_stream_indices) r.stream_index = remap[r.stream_index];
        r.order = next_order++;
        out.records.push_back(std::move(r));
    }
    if (out.records.empty()) throw std::runtime_error("No RECORD selected");

    return out;
}

std::string serialize_recon_payload(const ReconPayload &recon) {
    std::ostringstream oss;
    oss << std::setprecision(17);
    oss << "AVPACK_RECON_V1\n";
    oss << "GRID\t" << recon.nx << "\t" << recon.ny << "\n";
    for (const auto &s : recon.streams) {
        std::string bitmap_payload = "-";
        if (!s.shared_bitmap.empty()) {
            bitmap_payload = base64_encode(zlib_compress(
                std::string(reinterpret_cast<const char *>(s.shared_bitmap.data()), s.shared_bitmap.size())));
        }
        oss << "STREAM\t" << s.stream_index << "\t" << s.frame_count << "\t" << s.bits << "\t"
            << s.min_value << "\t" << s.max_value << "\t" << s.storage_shift
            << "\t" << (s.apply_shifted_log ? "shifted-log" : s.apply_asinh ? "asinh" : s.apply_log2 ? "log2p1" : "linear")
            << "\t" << s.log_offset
            << "\t" << bitmap_payload
            << "\t" << (s.template_b64.empty() ? "-" : s.template_b64)
            << "\t" << s.encoded_nx
            << "\t" << s.encoded_ny
            << "\t" << s.crop_nx
            << "\t" << s.crop_ny
            << "\t" << s.transform_scale << "\t" << s.transform_origin << "\t" << s.transform_span
            << "\n";
    }
    for (const auto &r : recon.records) {
        oss << "RECORD\t" << r.order << "\t" << r.stream_index << "\t" << r.frame_index << "\t"
            << r.file_index << "\t" << r.message_index << "\t"
            << tsv_escape(r.short_name) << "\t" << tsv_escape(r.type_of_level) << "\t"
            << tsv_escape(r.level) << "\t" << tsv_escape(r.data_date) << "\t"
            << tsv_escape(r.data_time) << "\t" << tsv_escape(r.step_range) << "\t"
            << tsv_escape(r.timestamp) << "\t" << tsv_escape(r.ensemble_number) << "\t"
            << tsv_escape(r.perturbation_number) << "\t" << r.frame_min << "\t" << r.frame_max
            << "\t" << (r.bitmap_present ? 1 : 0) << "\t" << r.template_b64 << "\t";
        if (r.bits_per_value > 0) oss << r.bits_per_value;
        else oss << "-1";
        oss << "\t0\t0\t" << r.binary_scale_shift << "\n";
    }
    return oss.str();
}

std::string build_minimal_metadata_json(const ReconPayload &recon, const std::string &selector) {
    const std::string recon_text = serialize_recon_payload(recon);
    const std::string recon_payload = base64_encode(zlib_compress(recon_text));
    std::ostringstream json;
    json << "{"
         << "\"schema\":\"avpack.v1\""
         << ",\"source\":\"avenc2grib-group-subset\""
         << ",\"grid\":{\"nx\":" << recon.nx << ",\"ny\":" << recon.ny << "}"
         << ",\"stream_count\":" << recon.streams.size()
         << ",\"record_count\":" << recon.records.size()
         << ",\"group_selector\":\"" << json_escape(selector.empty() ? "all" : selector) << "\""
         << ",\"reconstruct_format\":\"avpack.recon.tsv.zlib.base64.v1\""
         << ",\"reconstruct_payload\":\"" << recon_payload << "\""
         << "}";
    return json.str();
}

std::string build_avenc_comment_v2_binary(const ReconPayload &recon) {
    std::vector<uint8_t> payload;
    payload.reserve(64u + recon.streams.size() * 256u + recon.records.size() * 320u);
    const char magic[8] = {'A', 'V', '2', 'M', 'E', 'T', 'A', '\0'};
    payload.insert(payload.end(), magic, magic + 8);
    const bool has_asinh = std::any_of(recon.streams.begin(), recon.streams.end(),
        [](const StreamInfo &s) { return s.apply_asinh; });
    const bool extended_transform=std::any_of(recon.streams.begin(),recon.streams.end(),[](const StreamInfo& s){return s.apply_shifted_log||(s.apply_asinh&&s.transform_scale!=1);});
    append_u32le(payload, extended_transform?4u:has_asinh ? 3u : 2u); // preserve transform on MP4 extraction
    append_u32le(payload, 0u); // flags
    append_u32le(payload, static_cast<uint32_t>(recon.nx));
    append_u32le(payload, static_cast<uint32_t>(recon.ny));
    append_u32le(payload, static_cast<uint32_t>(recon.streams.size()));
    append_u32le(payload, static_cast<uint32_t>(recon.records.size()));
    append_u32le(payload, 0u); // reserved

    for (const auto &s : recon.streams) {
        std::string bitmap_payload;
        if (!s.shared_bitmap.empty()) {
            bitmap_payload = base64_encode(zlib_compress(
                std::string(reinterpret_cast<const char *>(s.shared_bitmap.data()), s.shared_bitmap.size())));
        }
        append_i32le(payload, s.stream_index);
        append_u32le(payload, static_cast<uint32_t>(s.frame_count));
        append_u32le(payload, static_cast<uint32_t>(s.bits));
        append_i32le(payload, s.storage_shift);
        append_f64le(payload, s.min_value);
        append_f64le(payload, s.max_value);
        append_u32le(payload, avenc_transform::mode(s.apply_log2,s.apply_asinh,s.apply_shifted_log,s.transform_scale));
        append_f64le(payload, s.log_offset);
        if(extended_transform){append_f64le(payload,s.transform_scale);append_f64le(payload,s.transform_origin);append_f64le(payload,s.transform_span);}
        append_u32le(payload, static_cast<uint32_t>(s.encoded_nx));
        append_u32le(payload, static_cast<uint32_t>(s.encoded_ny));
        append_u32le(payload, static_cast<uint32_t>(s.crop_nx));
        append_u32le(payload, static_cast<uint32_t>(s.crop_ny));
        append_string_u32le(payload, bitmap_payload);
        append_string_u32le(payload, s.template_b64);
    }

    for (const auto &r : recon.records) {
        append_u64le(payload, static_cast<uint64_t>(r.order));
        append_i32le(payload, r.stream_index);
        append_u32le(payload, static_cast<uint32_t>(r.frame_index));
        append_i32le(payload, r.file_index);
        append_u64le(payload, static_cast<uint64_t>(r.message_index));
        append_f64le(payload, r.frame_min);
        append_f64le(payload, r.frame_max);
        append_u32le(payload, r.bitmap_present ? 1u : 0u);
        append_i32le(payload, r.bits_per_value);
        append_i32le(payload, r.binary_scale_shift);
        append_string_u32le(payload, r.short_name);
        append_string_u32le(payload, r.type_of_level);
        append_string_u32le(payload, r.level);
        append_string_u32le(payload, r.data_date);
        append_string_u32le(payload, r.data_time);
        append_string_u32le(payload, r.step_range);
        append_string_u32le(payload, r.timestamp);
        append_string_u32le(payload, r.ensemble_number);
        append_string_u32le(payload, r.perturbation_number);
        append_string_u32le(payload, r.template_b64);
    }

    return "AVENCv2|" + base64_encode(zlib_compress(
        std::string(reinterpret_cast<const char *>(payload.data()), payload.size())));
}

std::string build_avenc_comment(const std::string &metadata_json, int version) {
    if (version == 2) {
        throw std::runtime_error("AVENCv2 comments must use binary metadata format version 2");
    }

    const std::string meta_payload = base64_encode(zlib_compress(metadata_json));
    return "AVENCv1|" + meta_payload;
}

void extract_mp4_subset(const Options &opts,
                        const std::vector<int> &selected_old_stream_indices,
                        const std::string &comment) {
    if (selected_old_stream_indices.empty()) throw std::runtime_error("No selected stream for MP4 extraction");

    const std::string tmp_mux = make_temp_path("avenc2grib_mux_", ".mp4");
    const std::string tmp_meta = make_temp_path("avenc2grib_meta_", ".txt");
    try {
        std::vector<std::string> copy_args = {
            "ffmpeg", "-y", "-loglevel", "error", "-i", opts.input_mp4
        };
        for (int idx : selected_old_stream_indices) {
            copy_args.push_back("-map");
            copy_args.push_back("0:v:" + std::to_string(idx));
        }
        copy_args.push_back("-c");
        copy_args.push_back("copy");
        copy_args.push_back("-map_metadata");
        copy_args.push_back("-1");
        copy_args.push_back(tmp_mux);

        int ret = run_command(copy_args, opts.verbose);
        if (ret != 0) {
            throw std::runtime_error("ffmpeg stream copy failed with code " + std::to_string(ret));
        }

        std::ostringstream meta;
        meta << ";FFMETADATA1\n";
        meta << "comment=" << ffmetadata_escape(comment) << "\n";
        write_text_file(tmp_meta, meta.str());

        std::vector<std::string> attach_args = {
            "ffmpeg", "-y", "-loglevel", "error", "-i", tmp_mux, "-i", tmp_meta,
            "-map", "0", "-map_metadata", "1", "-c", "copy", opts.output_path
        };
        ret = run_command(attach_args, opts.verbose);
        if (ret != 0) {
            throw std::runtime_error("ffmpeg metadata attach failed with code " + std::to_string(ret));
        }
    } catch (...) {
        std::remove(tmp_mux.c_str());
        std::remove(tmp_meta.c_str());
        throw;
    }
    std::remove(tmp_mux.c_str());
    std::remove(tmp_meta.c_str());
}

std::vector<uint16_t> decode_stream_quantized_range(const std::string &video_path,
                                                    const StreamInfo &stream,
                                                    size_t pixels_per_frame,
                                                    size_t frame_start,
                                                    size_t frame_count,
                                                    bool verbose,
                                                    const std::function<void(uint64_t)> &progress_cb = {}) {
    const int enc_nx = (stream.encoded_nx > 0) ? stream.encoded_nx : stream.crop_nx;
    const int enc_ny = (stream.encoded_ny > 0) ? stream.encoded_ny : stream.crop_ny;
    const int crop_nx = (stream.crop_nx > 0) ? stream.crop_nx : enc_nx;
    const int crop_ny = (stream.crop_ny > 0) ? stream.crop_ny : enc_ny;
    if (enc_nx <= 0 || enc_ny <= 0 || crop_nx <= 0 || crop_ny <= 0) {
        throw std::runtime_error("Invalid stream geometry in metadata");
    }
    if (frame_count == 0) return {};
    if (frame_start >= stream.frame_count || frame_start + frame_count > stream.frame_count) {
        throw std::runtime_error("Requested decode frame range is out of stream bounds");
    }
    const size_t enc_pixels_per_frame = static_cast<size_t>(enc_nx) * static_cast<size_t>(enc_ny);

    const size_t frame_end = frame_start + frame_count - 1;
    std::ostringstream cmd;
    cmd << "ffmpeg -loglevel error -i \"" << video_path << "\""
        << " -map 0:v:" << stream.stream_index
        << " -an -sn -fps_mode passthrough"
        << " -vf \"select='between(n\\," << frame_start << "\\," << frame_end << ")'\""
        << " -frames:v " << frame_count
        << " -f rawvideo -pix_fmt "
        << avenc_raw_gray_format(stream.bits, stream.storage_shift)
        << " -";

    FILE *pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) throw std::runtime_error("Failed to launch ffmpeg decode");

    const size_t bytes_per_px = (stream.bits <= 8) ? 1 : 2;
    const size_t out_values = frame_count * pixels_per_frame;
    const size_t frame_bytes = enc_pixels_per_frame * bytes_per_px;
    const size_t crop_values = static_cast<size_t>(crop_nx) * static_cast<size_t>(crop_ny);

    std::vector<uint16_t> out;
    try {
        out.assign(out_values, 0u);
    } catch (const std::bad_alloc &) {
        std::ostringstream oss;
        oss << "Unable to allocate quantized buffer for stream " << stream.stream_index
            << " (" << out_values << " values, " << (out_values * sizeof(uint16_t)) << " bytes)";
        throw std::runtime_error(oss.str());
    }

    std::vector<uint8_t> frame_raw(frame_bytes, 0u);
    const size_t crop_w = static_cast<size_t>(crop_nx);
    const size_t crop_h = static_cast<size_t>(crop_ny);
    const size_t enc_w = static_cast<size_t>(enc_nx);
    for (size_t f = 0; f < frame_count; ++f) {
        size_t got = 0;
        while (got < frame_bytes) {
            const size_t n = std::fread(frame_raw.data() + got, 1, frame_bytes - got, pipe);
            if (n == 0) break;
            got += n;
        }
        if (got != frame_bytes) {
            (void)pclose(pipe);
            std::ostringstream oss;
            oss << "Unexpected decoded stream size from ffmpeg at frame " << f
                << " (got " << got << " bytes, expected " << frame_bytes << ")";
            throw std::runtime_error(oss.str());
        }

        const size_t dst_base = f * pixels_per_frame;
        if (enc_nx == crop_nx && enc_ny == crop_ny && crop_values == pixels_per_frame) {
            if (stream.bits <= 8) {
                for (size_t i = 0; i < pixels_per_frame; ++i) out[dst_base + i] = frame_raw[i];
            } else {
                for (size_t i = 0; i < pixels_per_frame; ++i) {
                    out[dst_base + i] = static_cast<uint16_t>(
                        static_cast<uint16_t>(frame_raw[2 * i]) |
                        (static_cast<uint16_t>(frame_raw[2 * i + 1]) << 8));
                }
            }
        } else {
            for (size_t y = 0; y < crop_h; ++y) {
                const size_t src_row = y * enc_w;
                const size_t dst_row = dst_base + y * crop_w;
                if (stream.bits <= 8) {
                    for (size_t x = 0; x < crop_w; ++x) {
                        out[dst_row + x] = frame_raw[src_row + x];
                    }
                } else {
                    for (size_t x = 0; x < crop_w; ++x) {
                        const size_t si = (src_row + x) * 2;
                        out[dst_row + x] = static_cast<uint16_t>(
                            static_cast<uint16_t>(frame_raw[si]) |
                            (static_cast<uint16_t>(frame_raw[si + 1]) << 8));
                    }
                }
            }
        }
        if (progress_cb) progress_cb(1);
    }

    const int ret = pclose(pipe);
    if (ret != 0) throw std::runtime_error("ffmpeg stream decode failed");

    if (enc_nx == crop_nx && enc_ny == crop_ny && crop_values == pixels_per_frame) return out;

    // If crop dimensions differ from full logical frame size, out is already
    // zero-padded to pixels_per_frame per frame and populated in top-left crop.
    return out;
}

void write_grib_from_recon(const Options &opts, const ReconPayload &recon) {
    const size_t pixels = static_cast<size_t>(recon.nx) * static_cast<size_t>(recon.ny);
    const auto stream_lookup = build_stream_lookup(recon);
    if (stream_lookup.empty()) throw std::runtime_error("No stream metadata available for reconstruction");

    const size_t bytes_per_frame_q = pixels * sizeof(uint16_t);
    const size_t page_budget = std::max<size_t>(opts.page_size_bytes, bytes_per_frame_q);
    const size_t page_frames = std::max<size_t>(1, page_budget / std::max<size_t>(size_t(1), bytes_per_frame_q));

    struct StreamPageCache {
        size_t frame_start = 0;
        size_t frame_count = 0;
        uint64_t last_touch = 0;
        std::vector<uint16_t> q;
        bool contains(size_t frame_idx) const {
            return !q.empty() && frame_idx >= frame_start && frame_idx < (frame_start + frame_count);
        }
        size_t bytes() const { return q.size() * sizeof(uint16_t); }
        void clear() {
            q.clear();
            frame_start = 0;
            frame_count = 0;
            last_touch = 0;
        }
    };

    std::unordered_map<int, StreamPageCache> cache;
    cache.reserve(recon.streams.size());
    size_t cache_bytes = 0;
    uint64_t touch_clock = 1;

    auto evict_lru_other_than = [&](int protected_stream) -> bool {
        int victim_stream = -1;
        uint64_t oldest = std::numeric_limits<uint64_t>::max();
        for (auto &kv : cache) {
            if (kv.first == protected_stream) continue;
            if (kv.second.q.empty()) continue;
            if (kv.second.last_touch < oldest) {
                oldest = kv.second.last_touch;
                victim_stream = kv.first;
            }
        }
        if (victim_stream < 0) return false;
        auto &victim = cache[victim_stream];
        const size_t victim_bytes = victim.bytes();
        victim.clear();
        cache_bytes = (cache_bytes >= victim_bytes) ? (cache_bytes - victim_bytes) : 0;
        return true;
    };

    std::vector<RecordInfo> records = recon.records;
    std::sort(records.begin(), records.end(),
              [](const RecordInfo &a, const RecordInfo &b) { return a.order < b.order; });
    uint64_t total_decode_frames = 0;
    for (const auto &s : recon.streams) total_decode_frames += static_cast<uint64_t>(s.frame_count);
    const uint64_t total_units =
        static_cast<uint64_t>(records.size()) + total_decode_frames;
    uint64_t progress_done = 0;

    FILE *out = std::fopen(opts.output_path.c_str(), "wb");
    if (!out) throw std::runtime_error("Unable to open output GRIB: " + opts.output_path);

    try {
        ProgressBar global_progress("decode+write", std::max<uint64_t>(1, total_units), opts.verbose);
        auto add_progress = [&](uint64_t n) {
            if (n == 0) return;
            progress_done = std::min<uint64_t>(total_units, progress_done + n);
            global_progress.update(progress_done);
        };
        for (size_t ri = 0; ri < records.size(); ++ri) {
            const auto &r = records[ri];
            auto sit = stream_lookup.find(r.stream_index);
            if (sit == stream_lookup.end() || sit->second == nullptr) {
                throw std::runtime_error("Decoded stream metadata missing for stream index " +
                                         std::to_string(r.stream_index));
            }
            const auto &s = *(sit->second);
            if (r.frame_index >= s.frame_count) {
                throw std::runtime_error("Frame index out of range in reconstruction payload");
            }

            auto &page = cache[r.stream_index];
            if (!page.contains(r.frame_index)) {
                if (!page.q.empty()) {
                    const size_t old_bytes = page.bytes();
                    page.clear();
                    cache_bytes = (cache_bytes >= old_bytes) ? (cache_bytes - old_bytes) : 0;
                }

                const size_t page_start = (r.frame_index / page_frames) * page_frames;
                const size_t remaining = s.frame_count - page_start;
                const size_t page_len = std::min(page_frames, remaining);
                const size_t page_bytes = page_len * bytes_per_frame_q;

                while (cache_bytes + page_bytes > page_budget) {
                    if (!evict_lru_other_than(r.stream_index)) break;
                }

                page.q = decode_stream_quantized_range(
                    opts.input_mp4,
                    s,
                    pixels,
                    page_start,
                    page_len,
                    false,
                    [&](uint64_t n) { add_progress(n); });
                page.frame_start = page_start;
                page.frame_count = page_len;
                page.last_touch = touch_clock++;
                cache_bytes += page.bytes();
            } else {
                page.last_touch = touch_clock++;
            }

            const auto &q = page.q;
            const size_t local_frame = r.frame_index - page.frame_start;
            const size_t base = local_frame * pixels;
            const uint32_t qmax = (1u << s.bits) - 1u;
            double frame_min = r.frame_min;
            double frame_max = r.frame_max;
            if (!std::isfinite(frame_min) || !std::isfinite(frame_max) || frame_max < frame_min) {
                frame_min = s.min_value;
                frame_max = s.max_value;
            }
            double frame_span = frame_max - frame_min;
            // Zero span is an intentional constant-frame marker.
            const bool has_shared_bitmap = (s.shared_bitmap.size() == pixels);
            const bool has_bitmap = r.bitmap_present || has_shared_bitmap;
            const double qmax_d = static_cast<double>(qmax);
            const double qvalid_d = has_bitmap ? static_cast<double>(qmax - 1u) : qmax_d;

            std::string template_b64 = r.template_b64;
            if (template_b64.empty() || template_b64 == "-") {
                template_b64 = s.template_b64;
            }
            auto tmpl = base64_decode(template_b64);
            if (tmpl.empty()) throw std::runtime_error("Empty template payload");

            codes_handle *h = codes_handle_new_from_message_copy(nullptr, tmpl.data(), tmpl.size());
            if (!h) throw std::runtime_error("Failed to build GRIB handle from template");

            // Ensure per-record core header keys are restored even when template payload
            // is reused across time steps.
            set_key_from_text(h, "dataDate", r.data_date);
            set_key_from_text(h, "dataTime", r.data_time);
            set_key_from_text(h, "stepRange", r.step_range);
            set_key_from_text(h, "number", r.ensemble_number);
            set_key_from_text(h, "perturbationNumber", r.perturbation_number);
            set_validity_from_timestamp(h, r.timestamp);

            size_t nvals = 0;
            if (codes_get_size(h, "values", &nvals) != 0 || nvals != pixels) {
                codes_handle_delete(h);
                throw std::runtime_error("Template values size mismatch");
            }

            // Do not let the ecCodes default sentinel (9999) swallow valid data
            // while it constructs the output GRIB bitmap from this value array.
            const double missing_value = CODES_MISSING_DOUBLE;
            if (codes_set_double(h, "missingValue", missing_value) != 0) {
                codes_handle_delete(h);
                throw std::runtime_error("Failed to set GRIB missing-value sentinel");
            }

            std::vector<double> values(pixels);
            bool has_missing = false;
            for (size_t i = 0; i < pixels; ++i) {
                if (has_shared_bitmap && s.shared_bitmap[i] != 0u) {
                    values[i] = missing_value;
                    has_missing = true;
                    continue;
                }
                uint32_t qv = static_cast<uint32_t>(q[base + i]);
                if (qv == 0u && has_bitmap && !has_shared_bitmap) {
                    values[i] = missing_value;
                    has_missing = true;
                    continue;
                }
                if (s.storage_shift > 0) {
                    const uint32_t round = 1u << (static_cast<uint32_t>(s.storage_shift) - 1u);
                    qv = (qv + round) >> static_cast<uint32_t>(s.storage_shift);
                }
                if (qv > qmax) qv = qmax;
                double norm = 0.0;
                if (has_bitmap) {
                    if (qv <= 1u) norm = 0.0;
                    else norm = static_cast<double>(qv - 1u) / qvalid_d;
                } else {
                    norm = static_cast<double>(qv) / qmax_d;
                }
                if (norm < 0.0) norm = 0.0;
                if (norm > 1.0) norm = 1.0;
                double decoded = norm * frame_span + frame_min;
                values[i] = decode_transform(decoded, s.apply_log2, s.apply_asinh, s.log_offset, s.apply_shifted_log, s.transform_scale, s.transform_origin, s.transform_span);
            }

            if (has_missing) (void)codes_set_long(h, "bitmapPresent", 1);
            else (void)codes_set_long(h, "bitmapPresent", 0);

            if (codes_set_double_array(h, "values", values.data(), pixels) != 0) {
                codes_handle_delete(h);
                throw std::runtime_error("Failed to set values on reconstructed GRIB message");
            }

            if (r.bits_per_value > 0) {
                int bret = codes_set_long(h, "setBitsPerValue", static_cast<long>(r.bits_per_value));
                if (bret != 0) {
                    bret = codes_set_long(h, "bitsPerValue", static_cast<long>(r.bits_per_value));
                }
                if (bret != 0 && opts.verbose) {
                    std::cerr << "[warn] unable to set bitsPerValue=" << r.bits_per_value
                              << " for record order=" << r.order << "\n";
                }
            }

            const void *msg = nullptr;
            size_t msg_size = 0;
            if (codes_get_message(h, &msg, &msg_size) != 0 || msg == nullptr || msg_size == 0) {
                codes_handle_delete(h);
                throw std::runtime_error("Failed to serialize reconstructed GRIB message");
            }
            if (std::fwrite(msg, 1, msg_size, out) != msg_size) {
                codes_handle_delete(h);
                throw std::runtime_error("Failed to write reconstructed GRIB message");
            }

            codes_handle_delete(h);
            ++http_grib_messages_written;
            add_progress(1);
        }
        global_progress.finish();
    } catch (...) {
        std::fclose(out);
        throw;
    }
    std::fclose(out);
}

void write_zarr_from_recon(const Options &opts, const ReconPayload &recon) {
    namespace fs = std::filesystem;
    const size_t nx = static_cast<size_t>(recon.nx);
    const size_t ny = static_cast<size_t>(recon.ny);
    const size_t pixels = nx * ny;
    if (pixels == 0) throw std::runtime_error("Invalid geometry for Zarr output");

    const auto stream_lookup = build_stream_lookup(recon);
    if (stream_lookup.empty()) throw std::runtime_error("No streams in reconstruction payload");
    const bool use_gzip = (opts.zarr_compress == "gzip");
    const std::string compressor_json = use_gzip ? "{\"id\":\"gzip\",\"level\":6}" : "null";

    const fs::path root(opts.output_path);
    if (fs::exists(root)) throw std::runtime_error("Output path already exists: " + opts.output_path);
    fs::create_directories(root);
    write_text_file((root / ".zgroup").string(), "{\"zarr_format\":2}");
    write_text_file((root / ".zattrs").string(),
                    "{\"source\":\"avenc2grib\",\"input_mp4\":\"" + json_escape(opts.input_mp4) +
                    "\",\"nx\":" + std::to_string(recon.nx) +
                    ",\"ny\":" + std::to_string(recon.ny) + "}");

    std::unordered_map<int, std::unordered_map<size_t, const RecordInfo *>> rec_by_stream_frame;
    for (const auto &r : recon.records) rec_by_stream_frame[r.stream_index][r.frame_index] = &r;

    for (const auto &stream : recon.streams) {
        const auto sit = stream_lookup.find(stream.stream_index);
        if (sit == stream_lookup.end() || sit->second == nullptr) continue;
        const auto &frame_map = rec_by_stream_frame[stream.stream_index];
        const RecordInfo *sample = nullptr;
        if (!frame_map.empty()) sample = frame_map.begin()->second;
        const std::string short_name = sample ? sample->short_name : "var";
        const std::string lvl_name = sample ? sample->type_of_level : "level";
        const std::string base = "s" + std::to_string(stream.stream_index) + "_" +
                                 sanitize_token(short_name) + "_" + sanitize_token(lvl_name);
        const std::string tdim = "time_" + base;
        const size_t nt = stream.frame_count;
        const std::string shape3 = "[" + std::to_string(nt) + "," + std::to_string(ny) + "," + std::to_string(nx) + "]";
        const std::string chunks3 = "[1," + std::to_string(ny) + "," + std::to_string(nx) + "]";

        const fs::path data_dir = root / (base + "_data");
        const fs::path q_dir = root / (base + "_quantized");
        const fs::path t_dir = root / (base + "_time_ms");
        const fs::path fmin_dir = root / (base + "_frame_min");
        const fs::path fmax_dir = root / (base + "_frame_max");
        fs::create_directories(data_dir);
        fs::create_directories(q_dir);
        fs::create_directories(t_dir);
        fs::create_directories(fmin_dir);
        fs::create_directories(fmax_dir);

        write_text_file((data_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":" + shape3 + ",\"chunks\":" + chunks3 +
                        ",\"dtype\":\"<f4\",\"compressor\":" + compressor_json +
                        ",\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((data_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"" + tdim + "\",\"y\",\"x\"],"
                        "\"stream_index\":" + std::to_string(stream.stream_index) + ","
                        "\"shortName\":\"" + json_escape(short_name) + "\","
                        "\"typeOfLevel\":\"" + json_escape(lvl_name) + "\"}");

        write_text_file((q_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":" + shape3 + ",\"chunks\":" + chunks3 +
                        ",\"dtype\":\"<u2\",\"compressor\":" + compressor_json +
                        ",\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((q_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"" + tdim + "\",\"y\",\"x\"]}");

        write_text_file((t_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":[" + std::to_string(nt) + "],\"chunks\":[1],"
                        "\"dtype\":\"<i8\",\"compressor\":null,\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((t_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"" + tdim + "\"],"
                        "\"units\":\"milliseconds since 1970-01-01T00:00:00Z\"}");

        write_text_file((fmin_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":[" + std::to_string(nt) + "],\"chunks\":[1],"
                        "\"dtype\":\"<f8\",\"compressor\":null,\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((fmin_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"" + tdim + "\"]}");
        write_text_file((fmax_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":[" + std::to_string(nt) + "],\"chunks\":[1],"
                        "\"dtype\":\"<f8\",\"compressor\":null,\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((fmax_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"" + tdim + "\"]}");

        const uint32_t qmax = (1u << stream.bits) - 1u;
        const double qmax_d = static_cast<double>(qmax);
        const bool has_shared_bitmap = (stream.shared_bitmap.size() == pixels);

        for (size_t fi = 0; fi < nt; ++fi) {
            const auto q = decode_stream_quantized_range(opts.input_mp4, stream, pixels, fi, 1, false);
            const RecordInfo *r = nullptr;
            auto it = frame_map.find(fi);
            if (it != frame_map.end()) r = it->second;
            double fmin = (r && std::isfinite(r->frame_min)) ? r->frame_min : stream.min_value;
            double fmax = (r && std::isfinite(r->frame_max)) ? r->frame_max : stream.max_value;
            if (fmax < fmin) throw std::runtime_error("Reversed frame quantization bounds");
            const bool has_bitmap = has_shared_bitmap || (r && r->bitmap_present);
            const double qvalid_d = has_bitmap ? static_cast<double>(std::max<uint32_t>(1u, qmax - 1u)) : qmax_d;
            const double span = fmax - fmin;

            std::vector<float> decoded(pixels, std::numeric_limits<float>::quiet_NaN());
            for (size_t i = 0; i < pixels; ++i) {
                uint32_t qv = q[i];
                if (has_shared_bitmap && stream.shared_bitmap[i] != 0u) continue;
                if (has_bitmap && !has_shared_bitmap && qv == 0u) continue;
                if (stream.storage_shift > 0) {
                    const uint32_t round = 1u << (static_cast<uint32_t>(stream.storage_shift) - 1u);
                    qv = (qv + round) >> static_cast<uint32_t>(stream.storage_shift);
                }
                if (qv > qmax) qv = qmax;
                double norm = has_bitmap
                    ? ((qv <= 1u) ? 0.0 : static_cast<double>(qv - 1u) / qvalid_d)
                    : static_cast<double>(qv) / qmax_d;
                norm = std::max(0.0, std::min(1.0, norm));
                const double raw = fmin + norm * span;
                decoded[i] = static_cast<float>(decode_transform(raw, stream.apply_log2, stream.apply_asinh, stream.log_offset, stream.apply_shifted_log, stream.transform_scale, stream.transform_origin, stream.transform_span));
            }

            const std::string ck = std::to_string(fi) + ".0.0";
            const uint8_t *dptr = reinterpret_cast<const uint8_t *>(decoded.data());
            const size_t dbytes = decoded.size() * sizeof(float);
            if (use_gzip) {
                auto gz = gzip_compress_bytes(dptr, dbytes, 6);
                write_bytes_file((data_dir / ck).string(), gz.data(), gz.size());
            } else {
                write_bytes_file((data_dir / ck).string(), dptr, dbytes);
            }

            const uint8_t *qptr = reinterpret_cast<const uint8_t *>(q.data());
            const size_t qbytes = q.size() * sizeof(uint16_t);
            if (use_gzip) {
                auto gz = gzip_compress_bytes(qptr, qbytes, 6);
                write_bytes_file((q_dir / ck).string(), gz.data(), gz.size());
            } else {
                write_bytes_file((q_dir / ck).string(), qptr, qbytes);
            }

            const std::string ck1 = std::to_string(fi);
            const int64_t tms = (r ? parse_timestamp_ms(r->timestamp) : -1);
            write_bytes_file((t_dir / ck1).string(),
                             reinterpret_cast<const uint8_t *>(&tms), sizeof(int64_t));
            write_bytes_file((fmin_dir / ck1).string(),
                             reinterpret_cast<const uint8_t *>(&fmin), sizeof(double));
            write_bytes_file((fmax_dir / ck1).string(),
                             reinterpret_cast<const uint8_t *>(&fmax), sizeof(double));
        }
    }
}

void write_subset_zarr(const Options &opts, const ReconPayload &recon, int stream_index) {
    namespace fs = std::filesystem;
    const auto stream_lookup = build_stream_lookup(recon);
    auto sit = stream_lookup.find(stream_index);
    if (sit == stream_lookup.end() || sit->second == nullptr) {
        throw std::runtime_error("Unknown stream index for subset Zarr: " + std::to_string(stream_index));
    }
    const StreamInfo &stream = *(sit->second);

    const int full_nx = (stream.crop_nx > 0) ? stream.crop_nx : recon.nx;
    const int full_ny = (stream.crop_ny > 0) ? stream.crop_ny : recon.ny;
    if (full_nx <= 0 || full_ny <= 0) throw std::runtime_error("Invalid stream geometry");

    const int ni = std::max(0, opts.ni_start);
    const int nj = std::max(0, opts.nj_start);
    const int req_w = (opts.width > 0) ? opts.width : (full_nx - ni);
    const int req_h = (opts.height > 0) ? opts.height : (full_ny - nj);
    if (req_w <= 0 || req_h <= 0) throw std::runtime_error("Invalid subset width/height");
    if (ni + req_w > full_nx || nj + req_h > full_ny) {
        throw std::runtime_error("Subset window is out of stream bounds");
    }

    const int fs0 = std::max(0, opts.frame_start);
    const int max_count = static_cast<int>(stream.frame_count) - fs0;
    if (max_count <= 0) throw std::runtime_error("frame_start is out of bounds");
    const int fc = (opts.frame_count > 0) ? std::min(opts.frame_count, max_count) : max_count;

    float *decoded = nullptr;
    int out_w = 0;
    int out_h = 0;
    int out_t = 0;
    const int rc = avenc_decode_subset_to_float32(
        opts.input_mp4.c_str(),
        stream_index,
        ni,
        nj,
        req_w,
        req_h,
        fs0,
        fc,
        &decoded,
        &out_w,
        &out_h,
        &out_t);
    if (rc != 0) {
        const char *msg = avenc_decode_subset_last_error();
        throw std::runtime_error(msg && *msg ? msg : "subset decode failed");
    }

    try {
        if (!decoded || out_w <= 0 || out_h <= 0 || out_t <= 0) {
            throw std::runtime_error("Empty decoded subset");
        }
        const size_t sub_pixels = static_cast<size_t>(out_w) * static_cast<size_t>(out_h);
        const size_t full_pixels = static_cast<size_t>(full_nx) * static_cast<size_t>(full_ny);

        const auto qfull = decode_stream_quantized_range(
            opts.input_mp4, stream, full_pixels, static_cast<size_t>(fs0), static_cast<size_t>(out_t), false);
        std::vector<uint16_t> qsub(static_cast<size_t>(out_t) * sub_pixels, 0u);
        for (int t = 0; t < out_t; ++t) {
            const size_t src_base = static_cast<size_t>(t) * full_pixels;
            const size_t dst_base = static_cast<size_t>(t) * sub_pixels;
            for (int y = 0; y < out_h; ++y) {
                const size_t src_row = src_base + static_cast<size_t>(nj + y) * static_cast<size_t>(full_nx) + static_cast<size_t>(ni);
                const size_t dst_row = dst_base + static_cast<size_t>(y) * static_cast<size_t>(out_w);
                std::copy(qfull.begin() + static_cast<long>(src_row),
                          qfull.begin() + static_cast<long>(src_row + static_cast<size_t>(out_w)),
                          qsub.begin() + static_cast<long>(dst_row));
            }
        }

        std::unordered_map<size_t, const RecordInfo *> rec_by_frame;
        for (const auto &r : recon.records) {
            if (r.stream_index == stream_index) rec_by_frame[r.frame_index] = &r;
        }
        std::vector<double> frame_min(static_cast<size_t>(out_t), stream.min_value);
        std::vector<double> frame_max(static_cast<size_t>(out_t), stream.max_value);
        std::vector<int64_t> time_ms(static_cast<size_t>(out_t), -1);
        for (int t = 0; t < out_t; ++t) {
            const size_t fi = static_cast<size_t>(fs0 + t);
            auto it = rec_by_frame.find(fi);
            if (it == rec_by_frame.end() || it->second == nullptr) continue;
            const RecordInfo *r = it->second;
            if (std::isfinite(r->frame_min)) frame_min[static_cast<size_t>(t)] = r->frame_min;
            if (std::isfinite(r->frame_max)) frame_max[static_cast<size_t>(t)] = r->frame_max;
            time_ms[static_cast<size_t>(t)] = parse_timestamp_ms(r->timestamp);
        }

        const fs::path root(opts.zarr_out);
        if (fs::exists(root)) throw std::runtime_error("Output path already exists: " + opts.zarr_out);
        fs::create_directories(root);
        write_text_file((root / ".zgroup").string(), "{\"zarr_format\":2}");

        std::string short_name = "var";
        std::string type_of_level = "level";
        for (const auto &r : recon.records) {
            if (r.stream_index == stream_index) {
                short_name = r.short_name;
                type_of_level = r.type_of_level;
                break;
            }
        }
        std::ostringstream root_attrs;
        root_attrs << "{"
                   << "\"source\":\"avenc2grib-subset\","
                   << "\"input_mp4\":\"" << json_escape(opts.input_mp4) << "\","
                   << "\"stream_index\":" << stream_index << ","
                   << "\"shortName\":\"" << json_escape(short_name) << "\","
                   << "\"typeOfLevel\":\"" << json_escape(type_of_level) << "\","
                   << "\"ni_start\":" << ni << ",\"nj_start\":" << nj << ","
                   << "\"width\":" << out_w << ",\"height\":" << out_h << ","
                   << "\"frame_start\":" << fs0 << ",\"frame_count\":" << out_t << ","
                   << "\"bits\":" << stream.bits << ","
                   << "\"transform_scale\":" << stream.transform_scale << ",\"transform_origin\":" << stream.transform_origin << ",\"transform_span\":" << stream.transform_span << ",\"apply_shifted_log\":" << (stream.apply_shifted_log?"true":"false") << ",\"storage_shift\":" << stream.storage_shift
                   << "}";
        write_text_file((root / ".zattrs").string(), root_attrs.str());

        const bool use_gzip = (opts.zarr_compress == "gzip");
        const std::string compressor_json = use_gzip ? "{\"id\":\"gzip\",\"level\":6}" : "null";
        const std::string shape3 = "[" + std::to_string(out_t) + "," + std::to_string(out_h) + "," + std::to_string(out_w) + "]";
        const std::string chunk3 = "[1," + std::to_string(out_h) + "," + std::to_string(out_w) + "]";

        const fs::path data_dir = root / "data";
        const fs::path q_dir = root / "quantized";
        const fs::path t_dir = root / "time_ms";
        const fs::path fmin_dir = root / "frame_min";
        const fs::path fmax_dir = root / "frame_max";
        fs::create_directories(data_dir);
        fs::create_directories(q_dir);
        fs::create_directories(t_dir);
        fs::create_directories(fmin_dir);
        fs::create_directories(fmax_dir);

        write_text_file((data_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":" + shape3 + ",\"chunks\":" + chunk3 +
                        ",\"dtype\":\"<f4\",\"compressor\":" + compressor_json +
                        ",\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((data_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"time\",\"y\",\"x\"]}");

        write_text_file((q_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":" + shape3 + ",\"chunks\":" + chunk3 +
                        ",\"dtype\":\"<u2\",\"compressor\":" + compressor_json +
                        ",\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((q_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"time\",\"y\",\"x\"]}");

        write_text_file((t_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":[" + std::to_string(out_t) + "],\"chunks\":[1],"
                        "\"dtype\":\"<i8\",\"compressor\":null,\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((t_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"time\"],\"units\":\"milliseconds since 1970-01-01T00:00:00Z\"}");
        write_text_file((fmin_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":[" + std::to_string(out_t) + "],\"chunks\":[1],"
                        "\"dtype\":\"<f8\",\"compressor\":null,\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((fmin_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"time\"]}");
        write_text_file((fmax_dir / ".zarray").string(),
                        "{\"zarr_format\":2,\"shape\":[" + std::to_string(out_t) + "],\"chunks\":[1],"
                        "\"dtype\":\"<f8\",\"compressor\":null,\"fill_value\":null,\"order\":\"C\",\"filters\":null}");
        write_text_file((fmax_dir / ".zattrs").string(),
                        "{\"_ARRAY_DIMENSIONS\":[\"time\"]}");

        const float *fptr = decoded;
        for (int t = 0; t < out_t; ++t) {
            const std::string ck3 = std::to_string(t) + ".0.0";
            const size_t off = static_cast<size_t>(t) * sub_pixels;

            const uint8_t *dptr = reinterpret_cast<const uint8_t *>(fptr + off);
            const size_t dbytes = sub_pixels * sizeof(float);
            if (use_gzip) {
                auto gz = gzip_compress_bytes(dptr, dbytes, 6);
                write_bytes_file((data_dir / ck3).string(), gz.data(), gz.size());
            } else {
                write_bytes_file((data_dir / ck3).string(), dptr, dbytes);
            }

            const uint8_t *qptr = reinterpret_cast<const uint8_t *>(qsub.data() + off);
            const size_t qbytes = sub_pixels * sizeof(uint16_t);
            if (use_gzip) {
                auto gz = gzip_compress_bytes(qptr, qbytes, 6);
                write_bytes_file((q_dir / ck3).string(), gz.data(), gz.size());
            } else {
                write_bytes_file((q_dir / ck3).string(), qptr, qbytes);
            }

            const std::string ck1 = std::to_string(t);
            const int64_t tms = time_ms[static_cast<size_t>(t)];
            const double fmn = frame_min[static_cast<size_t>(t)];
            const double fmx = frame_max[static_cast<size_t>(t)];
            write_bytes_file((t_dir / ck1).string(),
                             reinterpret_cast<const uint8_t *>(&tms), sizeof(int64_t));
            write_bytes_file((fmin_dir / ck1).string(),
                             reinterpret_cast<const uint8_t *>(&fmn), sizeof(double));
            write_bytes_file((fmax_dir / ck1).string(),
                             reinterpret_cast<const uint8_t *>(&fmx), sizeof(double));
        }
    } catch (...) {
        avenc_free_float32(decoded);
        throw;
    }
    avenc_free_float32(decoded);
}

} // namespace

int main(int argc, char **argv) {
    if (avenc_print_version(argc, argv)) return 0;
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) usage(argv[0], 0);
    try {
        Options opts;
        parse_args(argc, argv, opts);
        AvencHttpInput input(opts.input_mp4, argv[0], opts.http_block_size, opts.http_timeout,
                             opts.http_fallback_limit, opts.http_fallback, opts.http_stats);
        opts.input_mp4 = input.path;
        remote_object_size = input.object_size;
        input.grib_messages_written = &http_grib_messages_written;
        if (subset_mode_requested(opts) && trim(opts.zarr_out).empty()) {
            float *out = nullptr;
            int out_w = 0;
            int out_h = 0;
            int out_t = 0;
            const int rc = avenc_decode_subset_to_float32(
                opts.input_mp4.c_str(),
                opts.stream_id,
                opts.ni_start,
                opts.nj_start,
                opts.width,
                opts.height,
                opts.frame_start,
                opts.frame_count,
                &out,
                &out_w,
                &out_h,
                &out_t);
            if (rc != 0) {
                const char *msg = avenc_decode_subset_last_error();
                throw std::runtime_error(msg && *msg ? msg : "subset decode failed");
            }
            if (opts.verbose) {
                const char *cmd = avenc_decode_subset_last_ffmpeg_command();
                if (cmd && *cmd) {
                    std::cerr << "[subset-ffmpeg] " << cmd << "\n";
                }
                std::cerr << "[subset] stream=" << opts.stream_id
                          << " frame_start=" << opts.frame_start
                          << " frame_count=" << out_t
                          << " ni_start=" << opts.ni_start
                          << " nj_start=" << opts.nj_start
                          << " width=" << out_w
                          << " height=" << out_h
                          << "\n";
            }

            const size_t n = static_cast<size_t>(out_w) * static_cast<size_t>(out_h) *
                             static_cast<size_t>(out_t);
            if (n > 0 && out) {
                const size_t wrote = std::fwrite(out, sizeof(float), n, stdout);
                if (wrote != n) {
                    avenc_free_float32(out);
                    throw std::runtime_error("Failed writing float32 subset data to stdout");
                }
                std::fflush(stdout);
            }
            avenc_free_float32(out);
            input.finish();
            return 0;
        }

        const size_t parse_rss_before = current_rss_bytes();
        const size_t parse_peak_before = peak_rss_bytes();
        const auto parse_t0 = std::chrono::steady_clock::now();
        const std::string comment = extract_comment(opts.input_mp4);
        const DecodedComment decoded = decode_avenc_comment(comment);
        ReconPayload recon;
        if (decoded.has_binary_recon) {
            recon = decoded.recon_binary;
        } else {
            const std::string &recon_format = decoded.recon_format;
            if (recon_format != "avpack.recon.tsv.zlib.base64.v1") {
                throw std::runtime_error("Unsupported reconstruction format: " + recon_format);
            }
            const std::string &recon_b64 = decoded.recon_payload;
            const auto recon_zip = base64_decode(recon_b64);
            const auto recon_text_bytes = zlib_decompress(recon_zip);
            const std::string recon_text(recon_text_bytes.begin(), recon_text_bytes.end());
            recon = parse_recon_payload(recon_text);
        }
        const auto parse_t1 = std::chrono::steady_clock::now();
        const size_t parse_rss_after = current_rss_bytes();
        const size_t parse_peak_after = peak_rss_bytes();
        const double parse_ms = std::chrono::duration<double, std::milli>(parse_t1 - parse_t0).count();

        if (opts.head) {
            print_metadata_head(decoded, recon);
            input.finish();
            return 0;
        }

        if (opts.stats) {
            print_stats(opts.input_mp4,
                        decoded,
                        recon,
                        parse_ms,
                        parse_rss_before,
                        parse_rss_after,
                        parse_peak_before,
                        parse_peak_after);
            input.finish();
            return 0;
        }

        const auto groups = build_groups(recon);

        if (opts.list) {
            if (trim(opts.group_selector).empty()) {
                print_groups(recon, groups);
            } else {
                const auto selected = resolve_selected_streams(recon, groups, opts.group_selector);
                print_group_records(recon, selected);
            }
            input.finish();
            return 0;
        }

        if (!trim(opts.zarr_out).empty()) {
            int stream_index = -1;
            if (opts.stream_id >= 0) stream_index = opts.stream_id;
            if (!trim(opts.group_selector).empty()) {
                const auto selected = resolve_selected_streams(recon, groups, opts.group_selector);
                if (selected.empty()) throw std::runtime_error("No stream matched --group selector");
                if (stream_index >= 0 && selected.count(stream_index) == 0) {
                    throw std::runtime_error("--stream-id is not part of the selected --group");
                }
                if (stream_index < 0) {
                    if (selected.size() != 1) {
                        throw std::runtime_error("--zarr-out subset requires exactly one stream; use --stream-id or a unique --group selector");
                    }
                    stream_index = *selected.begin();
                }
            }
            if (stream_index < 0) {
                if (recon.streams.size() == 1) stream_index = recon.streams.front().stream_index;
                else throw std::runtime_error("--zarr-out requires --stream-id or --group when multiple streams exist");
            }
            write_subset_zarr(opts, recon, stream_index);
            input.finish();
            return 0;
        }

        const auto selected = resolve_selected_streams(recon, groups, opts.group_selector);
        if (ends_with_icase(opts.output_path, ".mp4")) {
            ReconPayload filtered = filter_recon(recon, selected, true);
            std::string subset_comment;
            if (decoded.version == 2) {
                subset_comment = build_avenc_comment_v2_binary(filtered);
            } else {
                const std::string subset_json = build_minimal_metadata_json(filtered, opts.group_selector);
                subset_comment = build_avenc_comment(subset_json, decoded.version);
            }

            std::vector<int> selected_old_streams;
            for (const auto &s : recon.streams) {
                if (selected.count(s.stream_index)) selected_old_streams.push_back(s.stream_index);
            }
            extract_mp4_subset(opts, selected_old_streams, subset_comment);
            input.finish();
            return 0;
        }

        ReconPayload filtered = filter_recon(recon, selected, false);
        if (ends_with_icase(opts.output_path, ".zarr")) {
            write_zarr_from_recon(opts, filtered);
            input.finish();
            return 0;
        }
        write_grib_from_recon(opts, filtered);
        input.finish();
        return 0;
    } catch (const std::bad_alloc &) {
        std::cerr << "avenc2grib error: out of memory (std::bad_alloc)\n";
        std::cerr << "Hint: run `avenc2grib --stats <input.mp4>` and check per-stream frames/crop sizes.\n";
        std::cerr << "Hint: if this happens on large streams, decode one group at a time with `--group`.\n";
        return 1;
    } catch (const std::exception &ex) {
        std::cerr << "avenc2grib error: " << ex.what() << "\n";
        return 1;
    }
}
