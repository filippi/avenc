#include "avenc_transform.h"
#include "avenc_recipes.h"
#include "avenc_pixel_layout.h"
#include "avenc_version.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <functional>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <eccodes.h>
#include <zlib.h>
#include "avenc_bitmap.h"

namespace {

struct Options {
    std::vector<std::string> inputs;
    std::string output_path;
    std::string recipes_path;
    std::string group_by = "auto"; // auto|var|var-level|level
    std::string codec = "hevc";
    std::string quality = "high";
    int bits = 12;
    std::string mask_fill = "taper3"; // min|taper3 (shared GRIB bitmap only)
    std::string quantization = "auto"; // auto|frameMinMax|groupMinMax
    std::string transform = "linear"; // linear|log|asinh|shifted-log
    double asinh_scale=1.0,log_alpha=99.0;
    bool asinh_scale_set=false,log_alpha_set=false;
    std::string chroma = "420"; // 400|420
    std::string encoder;
    std::vector<std::string> ffmpeg_extra_args;
    int force_crf = -1; // <0 disabled
    int threads = 0; // <=0: ffmpeg default
    std::string preset; // low|medium|high|fast|apple
    bool headers_only_scan = true;
    bool fast_path = false;
    bool verbose = false;
    bool quiet = false;
    int metadata_version = 2; // 1=json(zlib+b64), 2=binary-struct(zlib+b64)
};

struct MessageMeta {
    int recipe_index=-1;
    std::string recipe_scope;
    Options encoding_options;
    size_t global_order = 0;
    int file_index = -1;
    size_t message_index = 0; // absolute index in file (0-based)
    long long file_offset = -1; // byte offset of GRIB message in source file
    std::string short_name;
    std::string raw_parameter_key; // independent of ecCodes parameter-name definitions
    std::string type_of_level;
    std::string level;
    std::string ensemble_number;
    std::string perturbation_number;
    std::string data_date;
    std::string data_time;
    std::string validity_date;
    std::string validity_time;
    std::string step_range;
    std::string timestamp;
    int nx = 0;
    int ny = 0;
    double lat_first = 0.0;
    double lat_last = 0.0;
    double lon_first = 0.0;
    double lon_last = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    bool bitmap_present = false;
    std::string bitmap_md5;
    long scanning_mode = 0;
    int bits_per_value = -1;
    double missing_value = std::numeric_limits<double>::quiet_NaN();
    double value_min = std::numeric_limits<double>::quiet_NaN();
    double value_max = std::numeric_limits<double>::quiet_NaN();
    bool apply_log2 = false;
    bool apply_asinh = false;
    bool apply_shifted_log = false;
    double transform_scale = 1.0, transform_origin = 0.0, transform_span = 1.0;
    double log_offset = 1.0;
    double quant_min = std::numeric_limits<double>::quiet_NaN();
    double quant_max = std::numeric_limits<double>::quiet_NaN();
    std::string template_b64;
};

struct Group {
    Options encoding_options;
    std::vector<uint8_t> shared_bitmap; // packed GRIB bits, 1 = valid
    std::string bitmap_md5;
    std::string key;
    std::string label;
    std::string short_name;
    std::string type_of_level;
    std::string level;
    int nx = 0;
    int ny = 0;
    double lat_first = 0.0;
    double lat_last = 0.0;
    double lon_first = 0.0;
    double lon_last = 0.0;
    double dx = 0.0;
    double dy = 0.0;

    std::vector<size_t> message_indices;       // indices into all_messages
    std::vector<size_t> message_indices_sorted; // same, sorted by timestamp

    size_t frame_start = 0;
    size_t frame_count = 0;
    double raw_min = std::numeric_limits<double>::infinity(),raw_max = -std::numeric_limits<double>::infinity();
    double value_min = std::numeric_limits<double>::infinity();
    double value_max = -std::numeric_limits<double>::infinity();
    bool apply_log2 = false;
    bool apply_asinh = false;
    bool apply_shifted_log = false;
    double transform_scale = 1.0, transform_origin = 0.0, transform_span = 1.0;
    double log_offset = 1.0;
};

struct GroupRuntimeInfo {
    bool apply_log2 = false;
    bool apply_asinh = false;
    bool apply_shifted_log = false;
    double transform_scale = 1.0, transform_origin = 0.0, transform_span = 1.0;
    double log_offset = 1.0;
    bool shared_bitmap_present = false;
    std::string shared_bitmap_payload;
    bool adaptive_group_minmax = false;
    int encoded_nx = 0;
    int encoded_ny = 0;
    int crop_nx = 0;
    int crop_ny = 0;
    std::vector<double> frame_quant_min;
    std::vector<double> frame_quant_max;
    std::vector<int> frame_shift_bits;
};

struct Assignment {
    int group_id = -1;
    size_t frame_index_in_group = 0;
};

struct RunStats {
    size_t message_count = 0;
    size_t stream_count = 0;
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    size_t metadata_json_bytes = 0;
    size_t metadata_comment_bytes = 0;
    uint64_t metadata_file_bytes = 0;
    uint64_t temp_raw_bytes = 0;
    uint64_t temp_group_video_bytes = 0;
    uint64_t temp_mux_bytes = 0;
    uint64_t temp_total_bytes = 0;
    double scan_sec = 0.0;
    double group_build_sec = 0.0;
    double sorting_sec = 0.0;
    double compression_sec = 0.0;
    std::string compressor;
    double compression_ratio = 0.0;
    double peak_rss_mb = 0.0;
    bool fast_path_enabled = true;
    uint64_t fast_path_frames = 0;
    uint64_t encoded_frames = 0;
    bool headers_only_scan = true;
    double template_scan_sec = 0.0;
    double template_deferred_sec = 0.0;
};

uint64_t file_size_bytes(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) == 0 && st.st_size >= 0) {
        return static_cast<uint64_t>(st.st_size);
    }
    return 0;
}

std::string format_hms(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    long long s = static_cast<long long>(std::llround(seconds));
    long long h = s / 3600;
    long long m = (s % 3600) / 60;
    long long sec = s % 60;
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << h
        << ":" << std::setw(2) << m
        << ":" << std::setw(2) << sec;
    return oss.str();
}

std::string format_bytes(uint64_t bytes) {
    static const char *suffix[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int idx = 0;
    while (value >= 1024.0 && idx < 4) {
        value /= 1024.0;
        ++idx;
    }
    std::ostringstream oss;
    if (idx == 0) {
        oss << static_cast<uint64_t>(value) << " " << suffix[idx];
    } else {
        oss << std::fixed << std::setprecision(2) << value << " " << suffix[idx];
    }
    return oss.str();
}

double peak_rss_mb() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0); // bytes -> MiB
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0; // KiB -> MiB
#endif
}

class ProgressBar {
public:
    ProgressBar(std::string label, uint64_t total_units, bool enabled)
        : label_(std::move(label)),
          total_(std::max<uint64_t>(1, total_units)),
          enabled_(enabled),
          start_(std::chrono::steady_clock::now()),
          last_draw_(start_) {
        if (enabled_) draw(true);
    }

    ~ProgressBar() {
        if (enabled_ && !finished_ && last_line_len_ > 0) {
            std::cerr << "\n";
        }
    }

    void update(uint64_t done_units) {
        if (!enabled_) return;
        done_ = std::min(done_units, total_);
        draw(false);
    }

    void increment(uint64_t delta = 1) {
        update(done_ + delta);
    }

    void finish() {
        if (!enabled_ || finished_) return;
        done_ = total_;
        draw(true);
        std::cerr << "\n";
        finished_ = true;
    }

private:
    void draw(bool force) {
        if (!enabled_) return;
        auto now = std::chrono::steady_clock::now();
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
        line << label_
             << " [" << bar << "] "
             << std::fixed << std::setprecision(1) << (progress * 100.0) << "% "
             << "(" << done_ << "/" << total_ << ") "
             << "ETA " << format_hms(eta_sec);

        const std::string txt = line.str();
        std::cerr << "\r" << txt;
        if (txt.size() < last_line_len_) {
            std::cerr << std::string(last_line_len_ - txt.size(), ' ');
        }
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

int storage_shift_for_bits(const Options& opts) {
    const bool ffv1 = opts.encoder == "ffv1" || (opts.encoder.empty() && opts.codec == "ffv1");
    if (ffv1) return 0;
    const int bits = opts.bits;
    if (bits > 8 && bits < 16) return 16 - bits;
    return 0;
}

void print_usage(std::ostream &os, const char *prog) {
    os << "avpack packs GRIB messages into MP4 streams using FFmpeg and embeds\n"
       << "AVENC metadata for later reconstruction.\n\n"
       << "Usage: " << prog << " [options] <grib1> <grib2> ...\n"
       << "  --out <packed.mp4>                  Output MP4\n"
       << "  --group-by <auto|var|var-level|level>\n"
       << "  --preset <low|medium|high|fast|apple>\n"
       << "  --codec <avc|hevc|av1|vp9|ffv1>\n"
       << "  --quality <lossless|veryhigh|high|average|fast|verylow|compatible|multiset>\n"
       << "  --bits <8|10|12|16>\n"
       << "  --transform <linear|log|asinh|shifted-log>    numerical transform (default: linear)\n"
       << "  --shifted-log                       log after stream min/max normalization\n"
       << "  --asinh-scale <positive>             asinh(x/scale), default 1\n"
       << "  --log-alpha <positive>               shifted-log curvature, default 99\n"
       << "  --log | --asinh | --linear        shortcuts; log2(1+x), asinh(x), or identity\n"
       << "  --quantization <auto|frameMinMax|groupMinMax>\n"
       << "  --mask-fill <min|taper3>           missing-cell fill (default: taper3 for shared masks)\n"
       << "  --chroma <400|420>\n"
       << "  --encoder <ffmpeg_encoder_name>      e.g. hevc_videotoolbox\n"
       << "  --ffopt <token>                      append raw ffmpeg output option token (repeatable)\n"
       << "  --force-crf <int>                    override generated -crf value\n"
       << "  --threads <N> | -threads <N>         ffmpeg thread count override\n"
       << "  --headers_only_scan                  scan/sort on headers, defer template extraction (default)\n"
       << "  --no_headers_only_scan               eager template extraction during scan\n"
       << "  --fast-path | --fast_path            enable packed-int fast path (default: disabled)\n"
       << "  --no-fast-path | --no_fast_path      disable packed-int fast path\n"
       << "  --metadata-version <1|2>            comment payload format (default: 2)\n"
       << "  --metadata-v1                       shortcut for --metadata-version 1\n"
       << "  --metadata-v2                       shortcut for --metadata-version 2\n"
       << "  --verbose, -v\n"
       << "  --quiet, -q\n"
       << "  --help, -h\n";
}

[[noreturn]] void usage(const char *prog) {
    print_usage(std::cerr, prog);
    std::exit(1);
}

[[noreturn]] void help(const char *prog) {
    std::cout << "  --recipes CSV  Apply matching per-field encoding overrides; unmatched fields use CLI defaults\n";
    print_usage(std::cout, prog);
    std::exit(0);
}

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

void apply_preset(Options &opts) {
    if (opts.preset.empty()) return;
    if (opts.preset == "low") {
        opts.codec = "avc";
        opts.quality = "average";
        opts.bits = 8;
    } else if (opts.preset == "medium") {
        opts.codec = "hevc";
        opts.quality = "average";
        opts.bits = 10;
    } else if (opts.preset == "high") {
        opts.codec = "hevc";
        opts.quality = "high";
        opts.bits = 12;
    } else if (opts.preset == "fast") {
        opts.codec = "avc";
        opts.quality = "fast";
        opts.bits = 8;
    } else if (opts.preset == "apple") {
        opts.codec = "hevc";
        opts.quality = "compatible";
        opts.bits = 10;
        opts.encoder = "hevc_videotoolbox";
    } else {
        throw std::runtime_error("Unsupported preset: " + opts.preset);
    }
}

void parse_args(int argc, char **argv, Options &opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            opts.output_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            help(argv[0]);
        } else if (arg == "--group-by" && i + 1 < argc) {
            opts.group_by = argv[++i];
        } else if (arg == "--recipes" && i + 1 < argc) {
            opts.recipes_path = argv[++i];
        } else if (arg == "--codec" && i + 1 < argc) {
            opts.codec = argv[++i];
        } else if (arg == "--quality" && i + 1 < argc) {
            opts.quality = argv[++i];
        } else if (arg == "--bits" && i + 1 < argc) {
            opts.bits = std::atoi(argv[++i]);
        } else if (arg == "--transform" && i + 1 < argc) {
            opts.transform = argv[++i];
        } else if (arg == "--asinh-scale" && i + 1 < argc) {
            size_t end=0;std::string value=argv[++i];opts.asinh_scale=std::stod(value,&end);
            if(end!=value.size())throw std::runtime_error("Invalid --asinh-scale");opts.asinh_scale_set=true;
        } else if (arg == "--log-alpha" && i + 1 < argc) {
            size_t end=0;std::string value=argv[++i];opts.log_alpha=std::stod(value,&end);
            if(end!=value.size())throw std::runtime_error("Invalid --log-alpha");opts.log_alpha_set=true;
        } else if (arg == "--log" || arg == "--asinh" || arg == "--linear" || arg == "--shifted-log") {
            opts.transform = arg.substr(2);
        } else if (arg == "--quantization" && i + 1 < argc) {
            opts.quantization = argv[++i];
        } else if (arg == "--mask-fill" && i + 1 < argc) {
            opts.mask_fill = argv[++i];
        } else if (arg == "--chroma" && i + 1 < argc) {
            opts.chroma = argv[++i];
        } else if (arg == "--encoder" && i + 1 < argc) {
            opts.encoder = argv[++i];
        } else if (arg == "--ffopt" && i + 1 < argc) {
            opts.ffmpeg_extra_args.emplace_back(argv[++i]);
        } else if (arg == "--force-crf" && i + 1 < argc) {
            opts.force_crf = std::atoi(argv[++i]);
        } else if ((arg == "--threads" || arg == "-threads") && i + 1 < argc) {
            opts.threads = std::atoi(argv[++i]);
        } else if (arg == "--preset" && i + 1 < argc) {
            opts.preset = argv[++i];
        } else if (arg == "--headers_only_scan") {
            opts.headers_only_scan = true;
        } else if (arg == "--no_headers_only_scan") {
            opts.headers_only_scan = false;
        } else if (arg == "--fast-path" || arg == "--fast_path") {
            opts.fast_path = true;
        } else if (arg == "--no_fast_path" || arg == "--no-fast-path") {
            opts.fast_path = false;
        } else if (arg == "--metadata-version" && i + 1 < argc) {
            opts.metadata_version = std::atoi(argv[++i]);
        } else if (arg == "--metadata-v1") {
            opts.metadata_version = 1;
        } else if (arg == "--metadata-v2") {
            opts.metadata_version = 2;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg == "--quiet" || arg == "-q") {
            opts.quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            usage(argv[0]);
        } else {
            opts.inputs.push_back(arg);
        }
    }

    if (opts.output_path.empty() || opts.inputs.empty()) usage(argv[0]);
    if (!(opts.bits == 8 || opts.bits == 10 || opts.bits == 12 || opts.bits == 16)) {
        throw std::runtime_error("bits must be 8, 10, 12, or 16");
    }
    if (opts.transform != "linear" && opts.transform != "log" && opts.transform != "asinh" && opts.transform != "shifted-log")
        throw std::runtime_error("transform must be linear, log, asinh, or shifted-log");
    avenc_transform::validate(opts.asinh_scale,0,1);avenc_transform::validate(opts.log_alpha,0,1);
    if(opts.asinh_scale_set&&opts.transform!="asinh")throw std::runtime_error("--asinh-scale requires --asinh");
    if(opts.log_alpha_set&&opts.transform!="shifted-log")throw std::runtime_error("--log-alpha requires --shifted-log");
    if (opts.transform == "shifted-log" && opts.metadata_version == 1)
        throw std::runtime_error("shifted-log requires binary metadata (--metadata-version 2)");
    if (opts.transform == "asinh" && opts.metadata_version == 1)
        throw std::runtime_error("asinh requires binary metadata (--metadata-version 2)");
    if (!(opts.quantization == "auto" ||
          opts.quantization == "frameMinMax" ||
          opts.quantization == "groupMinMax")) {
        throw std::runtime_error("quantization must be auto, frameMinMax, or groupMinMax");
    }
    if (!(opts.chroma == "400" || opts.chroma == "420")) {
        throw std::runtime_error("chroma must be 400 or 420");
    }
    if (opts.chroma == "420" && opts.bits > 12) {
        throw std::runtime_error("chroma=420 supports bits <= 12");
    }
    if (!(opts.metadata_version == 1 || opts.metadata_version == 2)) {
        throw std::runtime_error("metadata-version must be 1 or 2");
    }
    if (opts.force_crf < -1) {
        throw std::runtime_error("force-crf must be >= 0 (or omitted)");
    }
    if (opts.threads < 0) {
        throw std::runtime_error("threads must be >= 0");
    }
    std::sort(opts.inputs.begin(), opts.inputs.end());
    apply_preset(opts);
}

Options recipe_options(const Options& defaults,const avenc_recipes::Rule& rule) {
    Options effective=defaults;effective.recipes_path.clear();effective.preset.clear();
    const auto& m=rule.options;
    auto get=[&](const std::string& key){auto it=m.find(key);return it==m.end()?std::string():it->second;};
    if(!get("action").empty()&&get("action")!="encode")
        throw std::runtime_error("Recipe line "+std::to_string(rule.line)+" requests source GRIB retention; it cannot be encoded into MP4. Retain that field separately or remove the rule explicitly.");
    for(const std::string key:{"bits","gop","crf"})if(!get(key).empty()){
        size_t end=0;const auto n=std::stoll(get(key),&end);
        if(end!=get(key).size()||n>1000000||n<(key=="crf"?-1:1))throw std::runtime_error("Invalid recipe integer: "+key);
    }
    std::vector<std::string> args={"recipe"};
    for(const std::string key:{"codec","quality","encoder","bits","quantization","transform","chroma"})if(!get(key).empty()){args.push_back("--"+key);args.push_back(get(key));}
    if(!get("group_by").empty()){args.push_back("--group-by");args.push_back(get("group_by"));}
    const bool replace_video=m.count("codec")||m.count("encoder")||m.count("quality");
    if(replace_video){effective.ffmpeg_extra_args.clear();effective.force_crf=-1;}
    if(m.count("transform")){effective.asinh_scale_set=false;effective.log_alpha_set=false;effective.asinh_scale=1;effective.log_alpha=99;}
    const auto transform=m.count("transform")?get("transform"):effective.transform;
    if(!get("transform_parameter").empty()){
        if(transform=="asinh")args.insert(args.end(),{"--asinh-scale",get("transform_parameter")});
        else if(transform=="shifted-log")args.insert(args.end(),{"--log-alpha",get("transform_parameter")});
        else if(get("transform_parameter")!="1")throw std::runtime_error("transform_parameter only applies to asinh/shifted-log");
    }
    if(!get("crf").empty()&&get("crf")!="-1")args.insert(args.end(),{"--force-crf",get("crf")});
    std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());parse_args(int(argv.size()),argv.data(),effective);
    if(effective.group_by!="var"&&effective.group_by!="var-level"&&effective.group_by!="auto"&&effective.group_by!="level")throw std::runtime_error("Invalid recipe group_by");
    if(!get("gop").empty()){
        size_t end=0;int g=std::stoi(get("gop"),&end);if(end!=get("gop").size()||g<1)throw std::runtime_error("Invalid recipe GOP");
        if(effective.encoder!="ffv1")effective.ffmpeg_extra_args.insert(effective.ffmpeg_extra_args.end(),{"-g",std::to_string(g)});
    }
    if(effective.encoder=="libaom-av1")effective.ffmpeg_extra_args.insert(effective.ffmpeg_extra_args.end(),{"-cpu-used",effective.quality=="lossless"?"4":"6","-row-mt","1","-lag-in-frames","0","-aq-mode","0","-tile-columns","0"});
    return effective;
}

std::string get_codes_value_as_string(codes_handle *h, const std::string &key) {
    char s[256];
    size_t len = sizeof(s);
    int err = codes_get_string(h, key.c_str(), s, &len);
    if (err == 0) return std::string(s);
    long lval = 0;
    if (codes_get_long(h, key.c_str(), &lval) == 0) return std::to_string(lval);
    double dval = 0.0;
    if (codes_get_double(h, key.c_str(), &dval) == 0) {
        std::ostringstream oss;
        oss << std::setprecision(15) << dval;
        return oss.str();
    }
    return "";
}

int parse_step_hours(const std::string &step_range) {
    std::string s = trim(step_range);
    if (s.empty()) return 0;
    auto p = s.find('-');
    std::string use = (p == std::string::npos) ? s : s.substr(p + 1);
    try {
        return std::stoi(use);
    } catch (...) {
        return 0;
    }
}

std::string lower_copy(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

struct CrfBounds {
    bool known = false;
    int min = 0;
    int max = 0;
};

CrfBounds crf_bounds_for_options(const Options &opts) {
    const std::string codec = lower_copy(trim(opts.codec));
    const std::string encoder = lower_copy(trim(opts.encoder));

    // libx264/libx265 valid CRF range.
    if (codec == "avc" || codec == "h264" || codec == "hevc" || codec == "h265" ||
        encoder == "libx264" || encoder == "libx265" || encoder == "libh265") {
        return {true, 0, 51};
    }
    // AV1/VP9 CRF range.
    if (codec == "av1" || codec == "vp9" ||
        encoder == "libaom-av1" || encoder == "libsvtav1" || encoder == "libvpx-vp9") {
        return {true, 0, 63};
    }
    return {};
}

// Applied before range quantization; inverse is recorded per stream.
double encode_transform(double value, bool log, bool asinh, double offset,
                        bool shifted=false, double scale=1, double origin=0, double span=1) {
    double result=avenc_transform::forward(value,log,asinh,offset,shifted,scale,origin,span);
    if(std::isfinite(value)&&!std::isfinite(result))throw std::runtime_error("Numerical transform overflow; choose a larger scale");
    return result;
}

std::string make_iso_timestamp_core(const std::string &date,
                                    const std::string &time_hhmm,
                                    int step_hours) {
    if (date.size() != 8) return date;

    int year = std::stoi(date.substr(0, 4));
    int month = std::stoi(date.substr(4, 2));
    int day = std::stoi(date.substr(6, 2));
    int hour = 0;
    int minute = 0;
    if (!time_hhmm.empty()) {
        try {
            int t = std::stoi(time_hhmm);
            hour = t / 100;
            minute = t % 100;
        } catch (...) {
            hour = 0;
            minute = 0;
        }
    }
    hour += step_hours;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;

#if defined(__APPLE__) || defined(__linux__)
    std::time_t tv = timegm(&tm);
#else
    std::time_t tv = std::mktime(&tm);
#endif
    if (tv == -1) return date;

    std::tm out_tm{};
#if defined(_WIN32)
    gmtime_s(&out_tm, &tv);
#else
    gmtime_r(&tv, &out_tm);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &out_tm);
    return std::string(buf);
}

std::string make_iso_timestamp(const std::string &data_date,
                               const std::string &data_time,
                               const std::string &step_range,
                               const std::string &validity_date,
                               const std::string &validity_time) {
    // Prefer explicit validity time when available, fallback to reference+step.
    if (validity_date.size() == 8) {
        return make_iso_timestamp_core(validity_date, validity_time, 0);
    }
    return make_iso_timestamp_core(data_date, data_time, parse_step_hours(step_range));
}

std::string grid_key(const MessageMeta &m) {
    std::ostringstream oss;
    oss << m.nx << "x" << m.ny << "|"
        << std::setprecision(12)
        << m.lat_first << "," << m.lat_last << ","
        << m.lon_first << "," << m.lon_last;
    return oss.str();
}

std::string grouping_key(const MessageMeta &m, const std::string &mode) {
    const std::string ensemble = m.ensemble_number + "|" + m.perturbation_number;
    const std::string variable = m.short_name == "unknown"
        ? m.short_name + "|" + m.raw_parameter_key : m.short_name;
    if (mode == "var") {
        return variable + "|" + m.type_of_level + "|" + ensemble + "|" + grid_key(m);
    }
    if (mode == "level") {
        return m.type_of_level + "|" + m.level + "|" + ensemble + "|" + grid_key(m);
    }
    // var-level
    return variable + "|" + m.type_of_level + "|" + m.level + "|" + ensemble + "|" + grid_key(m);
}

std::string template_cache_key(const MessageMeta &m, codes_handle *h) {
    std::ostringstream oss;
    oss << m.short_name << "|"
        << m.raw_parameter_key << "|"
        << m.type_of_level << "|"
        << m.level << "|"
        << m.ensemble_number << "|"
        << m.perturbation_number << "|"
        << m.nx << "x" << m.ny << "|"
        << std::setprecision(12)
        << m.lat_first << "|" << m.lat_last << "|"
        << m.lon_first << "|" << m.lon_last << "|"
        << m.dx << "|" << m.dy << "|"
        << (m.bitmap_present ? 1 : 0) << "|"
        << m.bits_per_value << "|"
        << get_codes_value_as_string(h, "edition") << "|"
        << get_codes_value_as_string(h, "gridType") << "|"
        << get_codes_value_as_string(h, "packingType") << "|"
        << get_codes_value_as_string(h, "productDefinitionTemplateNumber") << "|"
        << get_codes_value_as_string(h, "timeRangeIndicator");
    return oss.str();
}

int run_command(const std::vector<std::string> &args, bool verbose) {
    if (args.empty()) return -1;
    (void)verbose;

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv.data());
        perror("execvp");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

std::string effective_chroma_for_group(const Options &opts, int nx, int ny) {
    const bool is_vp9 = (opts.codec == "vp9" || opts.encoder == "libvpx-vp9");
    const bool is_av1 = (opts.codec == "av1" || opts.encoder == "libaom-av1" || opts.encoder == "libsvtav1");
    if (opts.chroma != "420") return opts.chroma;
    // x264/x265 4:2:0 requires even dimensions; fall back to 4:0:0 for odd grids.
    if (!is_vp9 && !is_av1 && ((nx % 2) != 0 || (ny % 2) != 0)) return "400";
    return "420";
}

std::vector<std::pair<std::string, std::string>> encoder_options(const Options &opts,
                                                                 int nx,
                                                                 int ny) {
    std::vector<std::pair<std::string, std::string>> out;
    auto add = [&](const std::string &k, const std::string &v) { out.emplace_back(k, v); };
    const bool is_vp9 = (opts.codec == "vp9" || opts.encoder == "libvpx-vp9");
    const std::string selected_encoder = [&]() {
        if (!opts.encoder.empty()) return opts.encoder;
        if (opts.codec == "av1") return std::string("libsvtav1");
        if (opts.codec == "vp9") return std::string("libvpx-vp9");
        if (opts.codec == "hevc" || opts.codec == "h265") return std::string("libx265");
        if (opts.codec == "avc" || opts.codec == "h264") return std::string("libx264");
        return opts.codec;
    }();
    const bool is_av1 = (opts.codec == "av1" || selected_encoder == "libaom-av1" || selected_encoder == "libsvtav1");
    const bool is_svtav1 = (selected_encoder == "libsvtav1");
    const std::string effective_chroma = effective_chroma_for_group(opts, nx, ny);

    auto gray_pixfmt = [&](int bits) -> std::string {
        if (bits <= 8) return "gray";
        if (bits <= 10) return "gray10le";
        if (bits <= 12) return "gray12le";
        return "gray16le";
    };
    auto yuv420_pixfmt = [&](int bits) -> std::string {
        if (bits <= 8) return "yuv420p";
        if (bits <= 10) return "yuv420p10le";
        return "yuv420p12le";
    };
    auto yuv444_pixfmt = [&](int bits) -> std::string {
        if (bits <= 8) return "yuv444p";
        if (bits <= 10) return "yuv444p10le";
        return "yuv444p12le";
    };

    auto set_pixfmt = [&](int bits) {
        if (effective_chroma == "420") {
            // Keep grayscale source as input; ffmpeg injects neutral chroma when converting.
            add("-pix_fmt", yuv420_pixfmt(bits));
            return;
        }
        if (is_vp9) {
            // libvpx-vp9 does not support gray pixel formats.
            add("-pix_fmt", yuv444_pixfmt(bits));
            return;
        }
        add("-pix_fmt", gray_pixfmt(bits));
    };

    if (selected_encoder == "ffv1") {
        if (opts.quality != "lossless" || effective_chroma != "400")
            throw std::runtime_error("FFV1 requires --quality lossless --chroma 400");
        if (opts.force_crf >= 0)
            throw std::runtime_error("FFV1 is lossless and has no CRF; omit --force-crf");
        // FFV1 is intra-only; never forward x264/x265 preset or CRF options.
        add("-pix_fmt", "+" + gray_pixfmt(opts.bits));
        add("-level", "3");
        add("-coder", "1");
        add("-context", "1");
        add("-slicecrc", "1");
        add("-g", "1");
        if (opts.threads > 0) add("-threads", std::to_string(opts.threads));
        return out;
    }

    if (opts.quality == "lossless") {
        set_pixfmt(opts.bits);
        if (is_vp9) {
            add("-lossless", "1");
            add("-crf", "0");
            add("-deadline", "best");
            add("-cpu-used", "0");
            add("-lag-in-frames", "25");
            add("-aq-mode", "1");
            add("-tile-columns", "0");
            add("-frame-parallel", "0");
        } else if (is_av1) {
            add("-crf", "0");
            if (is_svtav1) {
                add("-preset", "0");
            } else {
                add("-cpu-used", "0");
                add("-usage", "good");
                add("-lag-in-frames", "25");
                add("-aq-mode", "1");
                add("-tile-columns", "1");
                add("-frame-parallel", "0");
                add("-row-mt", "1");
            }
            add("-b:v", "0");
        } else {
            add("-crf", "0");
            add("-preset", "veryslow");
        }
    } else if (opts.quality == "veryhigh") {
        set_pixfmt(opts.bits);
        if (is_vp9) {
            add("-crf", "5");
            add("-deadline", "best");
            add("-cpu-used", "0");
            add("-lag-in-frames", "25");
            add("-aq-mode", "1");
            add("-tile-columns", "0");
            add("-frame-parallel", "0");
        } else if (is_av1) {
            add("-crf", "12");
            if (is_svtav1) {
                add("-preset", "2");
            } else {
                add("-cpu-used", "1");
                add("-usage", "good");
                add("-lag-in-frames", "25");
                add("-aq-mode", "1");
                add("-tile-columns", "1");
                add("-frame-parallel", "0");
                add("-row-mt", "1");
            }
            add("-b:v", "0");
        } else {
            add("-crf", "5");
            add("-preset", "slow");
        }
    } else if (opts.quality == "high") {
        set_pixfmt(opts.bits);
        if (is_vp9) {
            add("-crf", "10");
            add("-deadline", "good");
            add("-cpu-used", "2");
            add("-lag-in-frames", "20");
            add("-aq-mode", "1");
            add("-tile-columns", "1");
            add("-frame-parallel", "0");
        } else if (is_av1) {
            add("-crf", "18");
            if (is_svtav1) {
                add("-preset", "4");
            } else {
                add("-cpu-used", "2");
                add("-usage", "good");
                add("-lag-in-frames", "20");
                add("-aq-mode", "1");
                add("-tile-columns", "1");
                add("-frame-parallel", "0");
                add("-row-mt", "1");
            }
            add("-b:v", "0");
        } else {
            add("-crf", "10");
            add("-preset", "medium");
        }
    } else if (opts.quality == "average") {
        set_pixfmt(opts.bits);
        if (is_vp9) {
            add("-crf", "20");
            add("-deadline", "good");
            add("-cpu-used", "4");
            add("-lag-in-frames", "16");
            add("-aq-mode", "1");
            add("-tile-columns", "1");
            add("-frame-parallel", "1");
        } else if (is_av1) {
            add("-crf", "28");
            if (is_svtav1) {
                add("-preset", "6");
            } else {
                add("-cpu-used", "4");
                add("-usage", "good");
                add("-lag-in-frames", "16");
                add("-aq-mode", "1");
                add("-tile-columns", "1");
                add("-frame-parallel", "1");
                add("-row-mt", "1");
            }
            add("-b:v", "0");
        } else {
            add("-crf", "20");
            add("-preset", "medium");
        }
    } else if (opts.quality == "fast") {
        set_pixfmt(opts.bits);
        if (is_vp9) {
            add("-crf", "28");
            add("-deadline", "realtime");
            add("-cpu-used", "7");
            add("-aq-mode", "0");
            add("-lag-in-frames", "0");
            add("-tile-columns", "2");
            add("-frame-parallel", "1");
            add("-bf", "0");
        } else if (is_av1) {
            add("-crf", "36");
            if (is_svtav1) {
                add("-preset", "10");
            } else {
                add("-cpu-used", "8");
                add("-usage", "realtime");
                add("-lag-in-frames", "0");
                add("-aq-mode", "0");
                add("-tile-columns", "2");
                add("-frame-parallel", "1");
                add("-row-mt", "1");
            }
            add("-b:v", "0");
        } else {
            add("-crf", "23");
            add("-preset", "ultrafast");
            add("-bf", "0");
        }
    } else if (opts.quality == "verylow") {
        set_pixfmt(opts.bits);
        if (is_vp9) {
            add("-crf", "42");
            add("-deadline", "realtime");
            add("-cpu-used", "8");
            add("-aq-mode", "0");
            add("-lag-in-frames", "0");
            add("-tile-columns", "3");
            add("-frame-parallel", "1");
            add("-bf", "0");
        } else if (is_av1) {
            add("-crf", "50");
            if (is_svtav1) {
                add("-preset", "12");
            } else {
                add("-cpu-used", "8");
                add("-usage", "realtime");
                add("-lag-in-frames", "0");
                add("-aq-mode", "0");
                add("-tile-columns", "3");
                add("-frame-parallel", "1");
                add("-row-mt", "1");
            }
            add("-b:v", "0");
        } else {
            add("-crf", "35");
            add("-preset", "veryfast");
            add("-bf", "0");
        }
    } else if (opts.quality == "compatible") {
        if (is_av1) {
            set_pixfmt(opts.bits);
            add("-crf", "30");
            if (is_svtav1) {
                add("-preset", "6");
            } else {
                add("-cpu-used", "4");
                add("-usage", "good");
                add("-lag-in-frames", "12");
                add("-aq-mode", "1");
                add("-tile-columns", "1");
                add("-frame-parallel", "1");
                add("-row-mt", "1");
            }
            add("-b:v", "0");
        } else {
            add("-crf", "20");
            add("-preset", "medium");
        }
    } else if (opts.quality == "multiset") {
        if (is_vp9 || is_av1) {
            throw std::runtime_error("Unsupported quality: multiset for vp9/av1");
        }
        set_pixfmt(opts.bits);
        add("-crf", "15");
        add("-g", "12");
        add("-preset", "medium");
    } else {
        throw std::runtime_error("Unsupported quality: " + opts.quality);
    }

    if (is_vp9) {
        add("-b:v", "0");
        add("-row-mt", "1");
        add("-color_range", "pc");
    }

    if (opts.force_crf >= 0) {
        int forced_crf = opts.force_crf;
        const CrfBounds bounds = crf_bounds_for_options(opts);
        if (bounds.known) {
            if (forced_crf < bounds.min) forced_crf = bounds.min;
            if (forced_crf > bounds.max) forced_crf = bounds.max;
            if (forced_crf != opts.force_crf) {
                std::cerr << "[warn] requested --force-crf " << opts.force_crf
                          << " is outside supported range [" << bounds.min << "," << bounds.max
                          << "] for codec=" << opts.codec
                          << (opts.encoder.empty() ? "" : " encoder=" + opts.encoder)
                          << "; clamping to " << forced_crf << "\n";
            }
        }
        const std::string forced = std::to_string(forced_crf);
        bool replaced = false;
        for (auto &kv : out) {
            if (kv.first == "-crf") {
                kv.second = forced;
                replaced = true;
            }
        }
        if (!replaced) {
            out.emplace_back("-crf", forced);
        }
    }
    if (opts.threads > 0) {
        out.emplace_back("-threads", std::to_string(opts.threads));
    }

    return out;
}

std::string ffmpeg_codec_name(const Options &opts) {
    if (!opts.encoder.empty()) {
        if (opts.encoder == "libh265") return "libx265"; // tolerate common typo/alias
        return opts.encoder;
    }
    if (opts.codec == "avc" || opts.codec == "h264") return "libx264";
    if (opts.codec == "hevc" || opts.codec == "h265") return "libx265";
    if (opts.codec == "vp9") return "libvpx-vp9";
    if (opts.codec == "av1") return "libsvtav1";
    return opts.codec;
}

std::string json_escape(const std::string &input) {
    std::ostringstream oss;
    for (char c : input) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c))
                        << std::dec;
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

std::string base64_encode(const std::vector<uint8_t> &data) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                          (static_cast<uint32_t>(data[i + 1]) << 8) |
                          static_cast<uint32_t>(data[i + 2]);
        encoded.push_back(table[(triple >> 18) & 0x3F]);
        encoded.push_back(table[(triple >> 12) & 0x3F]);
        encoded.push_back(table[(triple >> 6) & 0x3F]);
        encoded.push_back(table[triple & 0x3F]);
        i += 3;
    }

    if (i < data.size()) {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        bool two = false;
        if (i + 1 < data.size()) {
            triple |= static_cast<uint32_t>(data[i + 1]) << 8;
            two = true;
        }
        encoded.push_back(table[(triple >> 18) & 0x3F]);
        encoded.push_back(table[(triple >> 12) & 0x3F]);
        if (two) {
            encoded.push_back(table[(triple >> 6) & 0x3F]);
            encoded.push_back('=');
        } else {
            encoded.push_back('=');
            encoded.push_back('=');
        }
    }

    return encoded;
}

std::string zlib_base64_payload(const std::vector<uint8_t> &bytes) {
    z_stream strm{};
    if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK)
        throw std::runtime_error("deflateInit failed");

    std::vector<uint8_t> output;
    output.reserve(bytes.size() / 2 + 64);
    std::vector<uint8_t> chunk(1 << 14);

    strm.next_in = const_cast<Bytef *>(bytes.data());
    strm.avail_in = static_cast<uInt>(bytes.size());

    int ret;
    do {
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        ret = deflate(&strm, strm.avail_in ? Z_NO_FLUSH : Z_FINISH);
        size_t have = chunk.size() - strm.avail_out;
        output.insert(output.end(), chunk.data(), chunk.data() + have);
    } while (ret != Z_STREAM_END);

    deflateEnd(&strm);
    return base64_encode(output);
}

std::string encode_float_array_payload(const std::vector<float> &values) {
    std::vector<uint8_t> bytes(values.size() * sizeof(float));
    if (!bytes.empty()) std::memcpy(bytes.data(), values.data(), bytes.size());

    std::string payload = zlib_base64_payload(bytes);

    std::ostringstream oss;
    oss << "{\"encoding\":\"zlib+base64\",\"dtype\":\"float32\",\"shape\":["
        << values.size() << "],\"data\":\"" << payload << "\"}";
    return oss.str();
}

std::string encode_time_payload(const std::vector<std::string> &values) {
    std::ostringstream oss;
    oss << "{\"encoding\":\"python-list\",\"dtype\":\"str\",\"shape\":["
        << values.size() << "],\"values\":[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) oss << ',';
        oss << '"' << json_escape(values[i]) << '"';
    }
    oss << "]}";
    return oss.str();
}

std::string tsv_escape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

std::string extract_json_string_field(const std::string &json, const std::string &key) {
    const std::string pat = "\"" + key + "\":\"";
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
    static_assert(sizeof(double) == sizeof(uint64_t), "Unexpected double size");
    std::memcpy(&bits, &v, sizeof(bits));
    append_u64le(out, bits);
}

uint32_t to_u32_size(size_t n, const char *what) {
    if (n > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error(std::string("Value too large for u32 in ") + what);
    }
    return static_cast<uint32_t>(n);
}

void append_string_u32(std::vector<uint8_t> &out, const std::string &s) {
    append_u32le(out, to_u32_size(s.size(), "string"));
    out.insert(out.end(), s.begin(), s.end());
}

std::string compress_comment_json_v1(const std::string &json) {
    std::vector<uint8_t> bytes(json.begin(), json.end());
    const std::string b64 = zlib_base64_payload(bytes);
    return "AVENCv1|" + b64;
}

std::string write_ffmetadata_file(const std::string &comment) {
    char path_template[] = "/tmp/avpack_metaXXXXXX";
    int fd = mkstemp(path_template);
    if (fd == -1) return "";

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        std::remove(path_template);
        return "";
    }

    const std::string header = ";FFMETADATA1\n";
    if (fwrite(header.data(), 1, header.size(), fp) != header.size()) {
        std::fclose(fp);
        std::remove(path_template);
        return "";
    }

    std::string line = "comment=" + comment + "\n";
    if (fwrite(line.data(), 1, line.size(), fp) != line.size()) {
        std::fclose(fp);
        std::remove(path_template);
        return "";
    }

    if (std::fclose(fp) != 0) {
        std::remove(path_template);
        return "";
    }

    return std::string(path_template);
}

bool field_missing(double value, const std::vector<uint8_t>& bitmap, size_t i) {
    if (avenc_bitmap::missing(bitmap, i)) return true;
    if (!std::isfinite(value))
        throw std::runtime_error("Non-finite GRIB value outside the source bitmap");
    return false;
}

int detect_padding_alignment(const Options &opts) {
    const char *env_align = std::getenv("AVPACK_PAD_ALIGN");
    if (env_align && *env_align) {
        int a = std::atoi(env_align);
        if (a >= 1) return a;
    }
    (void)opts;
    // Default to even dimensions per group (hardware-friendly, metadata carries crop).
    return 2;
}

int align_up_to(int value, int align) {
    if (align <= 1) return value;
    return ((value + align - 1) / align) * align;
}

struct FastPathPayload {
    bool ok = false;
    std::vector<uint32_t> q_values;
    double frame_min = std::numeric_limits<double>::quiet_NaN();
    double frame_max = std::numeric_limits<double>::quiet_NaN();
    int shift_bits = 0;
};

FastPathPayload try_extract_simple_packed_fastpath(const Options &opts,
                                                   const GroupRuntimeInfo &runtime_info,
                                                   const Group &group,
                                                   codes_handle *h,
                                                   size_t pixels) {
    FastPathPayload fp;
    if (!h || !opts.fast_path || runtime_info.apply_log2 || runtime_info.apply_asinh || runtime_info.apply_shifted_log || runtime_info.adaptive_group_minmax) return fp;

    long bit_map_indicator = -1;
    if (codes_get_long(h, "bitMapIndicator", &bit_map_indicator) != 0 || bit_map_indicator != 255) {
        return fp;
    }

    long scanning_mode = -1;
    if (codes_get_long(h, "scanningMode", &scanning_mode) != 0 || scanning_mode != 0) {
        return fp;
    }

    char packing_type[64];
    size_t packing_len = sizeof(packing_type);
    if (codes_get_string(h, "packingType", packing_type, &packing_len) != 0) return fp;
    if (std::string(packing_type) != "grid_simple") return fp;

    long edition = 0;
    if (codes_get_long(h, "edition", &edition) != 0) return fp;
    if (edition == 2) {
        long drt = -1;
        if (codes_get_long(h, "dataRepresentationTemplateNumber", &drt) != 0 || drt != 0) {
            return fp;
        }
    }

    long src_bits = 0;
    if (codes_get_long(h, "bitsPerValue", &src_bits) != 0) return fp;
    // Restrict fast-path to packed 16-bit payloads.
    if (src_bits != 16) return fp;

    double ref_value = 0.0;
    long bin_scale = 0;
    long dec_scale = 0;
    if (codes_get_double(h, "referenceValue", &ref_value) != 0) return fp;
    if (codes_get_long(h, "binaryScaleFactor", &bin_scale) != 0) return fp;
    if (codes_get_long(h, "decimalScaleFactor", &dec_scale) != 0) return fp;

    const void *msg = nullptr;
    size_t msg_size = 0;
    if (codes_get_message(h, &msg, &msg_size) != 0 || !msg || msg_size == 0) return fp;
    const auto *bytes = reinterpret_cast<const uint8_t *>(msg);

    size_t data_offset = std::numeric_limits<size_t>::max();
    long offset_values = -1;
    if (codes_get_long(h, "offsetValues", &offset_values) == 0 && offset_values >= 0) {
        data_offset = static_cast<size_t>(offset_values);
    } else {
        // Compatibility fallback for builds where offsetValues is unavailable.
        long offset_sec = -1;
        long len_sec = -1;
        long data_header_bytes = -1;
        if (edition == 1) {
            if (codes_get_long(h, "offsetSection4", &offset_sec) != 0) return fp;
            if (codes_get_long(h, "section4Length", &len_sec) != 0) return fp;
            data_header_bytes = 11;
        } else if (edition == 2) {
            if (codes_get_long(h, "offsetSection7", &offset_sec) != 0) return fp;
            if (codes_get_long(h, "section7Length", &len_sec) != 0) return fp;
            data_header_bytes = 5;
        } else {
            return fp;
        }
        if (offset_sec < 0 || len_sec <= data_header_bytes) return fp;
        data_offset = static_cast<size_t>(offset_sec + data_header_bytes);
    }
    const size_t packed_bytes_needed = pixels * sizeof(uint16_t);
    if (data_offset >= msg_size || data_offset + packed_bytes_needed > msg_size) return fp;
    const uint8_t *payload = bytes + data_offset;

    const int shift_bits = 16 - opts.bits;
    if (shift_bits < 0) return fp;
    const uint32_t qmax = (1u << static_cast<uint32_t>(opts.bits)) - 1u;
    fp.q_values.resize(pixels, 0u);
    for (size_t i = 0; i < pixels; ++i) {
        // GRIB payload is big-endian.
        const uint16_t be = static_cast<uint16_t>(
            (static_cast<uint16_t>(payload[i * 2]) << 8) |
            static_cast<uint16_t>(payload[i * 2 + 1]));
        uint32_t q = static_cast<uint32_t>(be);
        if (shift_bits > 0) q >>= static_cast<uint32_t>(shift_bits);
        if (q > qmax) q = qmax;
        fp.q_values[i] = q;
    }

    const double dec_scale_factor = std::pow(10.0, static_cast<double>(dec_scale));
    const double frame_min = ref_value / dec_scale_factor;
    const long adjusted_bin_scale = bin_scale + static_cast<long>(shift_bits);
    const double frame_max =
        (ref_value + static_cast<double>(qmax) * std::pow(2.0, static_cast<double>(adjusted_bin_scale))) /
        dec_scale_factor;

    // Guard against pathological simple-packing ranges (e.g. constant frames):
    // if the theoretical packed span is far larger than the actual data span,
    // tiny codec perturbations in q can explode in physical units.
    double hdr_min = std::numeric_limits<double>::quiet_NaN();
    double hdr_max = std::numeric_limits<double>::quiet_NaN();
    if (codes_get_double(h, "minimum", &hdr_min) == 0 &&
        codes_get_double(h, "maximum", &hdr_max) == 0 &&
        std::isfinite(hdr_min) && std::isfinite(hdr_max)) {
        const double hdr_span = hdr_max - hdr_min;
        const double packed_span = frame_max - frame_min;
        if (!(hdr_span > 0.0)) return fp;
        if (!(packed_span > 0.0)) return fp;
        if (packed_span > (hdr_span * 16.0)) return fp;
    }

    fp.ok = std::isfinite(frame_min) && std::isfinite(frame_max) && frame_max > frame_min;
    fp.frame_min = frame_min;
    fp.frame_max = frame_max;
    fp.shift_bits = shift_bits;
    (void)group;
    return fp;
}

void pack_with_padding_8bit(const std::vector<uint32_t> &src_q,
                            int nx,
                            int ny,
                            int enc_nx,
                            int enc_ny,
                            std::vector<uint8_t> &dst) {
    const size_t enc_pixels = static_cast<size_t>(enc_nx) * static_cast<size_t>(enc_ny);
    dst.assign(enc_pixels, 0u);
    for (int y = 0; y < ny; ++y) {
        const size_t src_row = static_cast<size_t>(y) * static_cast<size_t>(nx);
        const size_t dst_row = static_cast<size_t>(y) * static_cast<size_t>(enc_nx);
        for (int x = 0; x < nx; ++x) {
            dst[dst_row + static_cast<size_t>(x)] = static_cast<uint8_t>(src_q[src_row + static_cast<size_t>(x)]);
        }
    }
}

void pack_with_padding_16bit(const std::vector<uint32_t> &src_q,
                             int nx,
                             int ny,
                             int enc_nx,
                             int enc_ny,
                             int storage_shift,
                             std::vector<uint16_t> &dst) {
    const size_t enc_pixels = static_cast<size_t>(enc_nx) * static_cast<size_t>(enc_ny);
    dst.assign(enc_pixels, 0u);
    for (int y = 0; y < ny; ++y) {
        const size_t src_row = static_cast<size_t>(y) * static_cast<size_t>(nx);
        const size_t dst_row = static_cast<size_t>(y) * static_cast<size_t>(enc_nx);
        for (int x = 0; x < nx; ++x) {
            uint32_t q = src_q[src_row + static_cast<size_t>(x)];
            if (storage_shift > 0 && q > 0u) {
                q = std::min<uint32_t>(65535u, q << static_cast<uint32_t>(storage_shift));
            }
            dst[dst_row + static_cast<size_t>(x)] = static_cast<uint16_t>(q);
        }
    }
}

std::string make_template_payload_base64(codes_handle *h, size_t nvals) {
    codes_handle *clone = codes_handle_clone(h);
    if (!clone) {
        const void *msg0 = nullptr;
        size_t size0 = 0;
        if (codes_get_message(h, &msg0, &size0) == 0 && msg0 != nullptr && size0 > 0) {
            std::vector<uint8_t> bytes(size0);
            std::memcpy(bytes.data(), msg0, size0);
            return base64_encode(bytes);
        }
        return "";
    }

    // Build a compact metadata template with constant values.
    std::vector<double> zeros(nvals, 0.0);
    (void)codes_set_long(clone, "bitmapPresent", 0);
    (void)codes_set_double_array(clone, "values", zeros.data(), nvals);

    const void *msg = nullptr;
    size_t msg_size = 0;
    int err = codes_get_message(clone, &msg, &msg_size);
    if (err != 0 || msg == nullptr || msg_size == 0) {
        codes_handle_delete(clone);
        const void *msg0 = nullptr;
        size_t size0 = 0;
        if (codes_get_message(h, &msg0, &size0) == 0 && msg0 != nullptr && size0 > 0) {
            std::vector<uint8_t> bytes(size0);
            std::memcpy(bytes.data(), msg0, size0);
            return base64_encode(bytes);
        }
        return "";
    }

    std::vector<uint8_t> bytes(msg_size);
    std::memcpy(bytes.data(), msg, msg_size);
    codes_handle_delete(clone);
    return base64_encode(bytes);
}

std::string sanitize_name(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) return "group";
    return out;
}

std::string group_label(const MessageMeta &m, const std::string &mode) {
    std::ostringstream oss;
    oss << sanitize_name(m.short_name);
    if (!m.type_of_level.empty()) oss << "_" << sanitize_name(m.type_of_level);
    if (mode != "var" && !m.level.empty()) oss << "_" << sanitize_name(m.level);
    if (!m.ensemble_number.empty() && m.ensemble_number != "NOTFOUND") {
        oss << "_ens" << sanitize_name(m.ensemble_number);
    }
    if (!m.perturbation_number.empty() && m.perturbation_number != "NOTFOUND") {
        oss << "_pert" << sanitize_name(m.perturbation_number);
    }
    return oss.str();
}

std::vector<float> build_latitudes(const Group &g) {
    std::vector<float> lats(static_cast<size_t>(g.ny));
    double step = (g.ny > 1) ? ((g.lat_last - g.lat_first) / static_cast<double>(g.ny - 1)) : 0.0;
    for (int j = 0; j < g.ny; ++j) lats[static_cast<size_t>(j)] = static_cast<float>(g.lat_first + step * j);
    return lats;
}

std::vector<float> build_longitudes(const Group &g) {
    std::vector<float> lons(static_cast<size_t>(g.nx));
    double step = (g.nx > 1) ? ((g.lon_last - g.lon_first) / static_cast<double>(g.nx - 1)) : 0.0;
    for (int i = 0; i < g.nx; ++i) lons[static_cast<size_t>(i)] = static_cast<float>(g.lon_first + step * i);
    return lons;
}

std::string detect_group_mode(const std::vector<MessageMeta> &messages, const std::string &requested) {
    if (requested != "auto") return requested;

    const std::vector<std::string> candidates = {"var", "var-level", "level"};

    double best_score = std::numeric_limits<double>::infinity();
    std::string best = "var-level";

    for (const auto &mode : candidates) {
        std::map<std::string, std::vector<size_t>> grouped;
        for (size_t i = 0; i < messages.size(); ++i) {
            grouped[grouping_key(messages[i], mode)].push_back(i);
        }

        double score = static_cast<double>(grouped.size());
        if (mode == "var") score -= 25.0;
        for (const auto &kv : grouped) {
            std::set<std::string> vars;
            std::set<std::string> levels;
            std::set<std::string> times;
            std::set<std::string> time_level_pairs;
            size_t dup_times = 0;
            for (size_t idx : kv.second) {
                const auto &m = messages[idx];
                vars.insert(m.short_name);
                levels.insert(m.type_of_level + ":" + m.level);
                if (!times.insert(m.timestamp).second) {
                    if (mode == "var") {
                        std::string pair = m.timestamp + "|" + m.type_of_level + "|" + m.level;
                        if (!time_level_pairs.insert(pair).second) ++dup_times;
                    } else {
                        ++dup_times;
                    }
                } else if (mode == "var") {
                    std::string pair = m.timestamp + "|" + m.type_of_level + "|" + m.level;
                    time_level_pairs.insert(pair);
                }
            }
            if (vars.size() > 1) score += 1000.0;
            if (mode != "var" && levels.size() > 1) score += 200.0;
            score += static_cast<double>(dup_times) * 100.0;
        }

        if (score < best_score) {
            best_score = score;
            best = mode;
        }
    }

    return best;
}

struct ScanProfile {
    double template_sec = 0.0;
    double fallback_minmax_sec = 0.0;
    size_t fallback_minmax_count = 0;
    size_t template_cache_size = 0;
};

void scan_messages(const Options &opts,
                   std::vector<MessageMeta> &messages,
                   double &global_min,
                   double &global_max,
                   uint64_t total_input_bytes,
                   ScanProfile *profile = nullptr,
                   const std::function<void(uint64_t, uint64_t)> &progress_cb = {}) {
    using clock = std::chrono::steady_clock;
    const avenc_recipes::Table recipes(opts.recipes_path);
    std::map<int,Options> recipe_settings;
    global_min = std::numeric_limits<double>::infinity();
    global_max = -std::numeric_limits<double>::infinity();
    const bool skip_template = (std::getenv("AVPACK_SKIP_TEMPLATE") != nullptr);
    const bool allow_fallback_minmax = (std::getenv("AVPACK_MINMAX_FALLBACK_DECODE") != nullptr);
    const auto scan_t0 = clock::now();
    double template_sec = 0.0;
    double fallback_minmax_sec = 0.0;
    size_t fallback_minmax_count = 0;
    std::unordered_map<std::string, std::string> template_cache;

    uint64_t bytes_done = 0;
    for (size_t fi = 0; fi < opts.inputs.size(); ++fi) {
        const std::string &path = opts.inputs[fi];
        const uint64_t file_bytes = file_size_bytes(path);
        FILE *fp = std::fopen(path.c_str(), "rb");
        if (!fp) throw std::runtime_error("Unable to open GRIB file: " + path);

        int err = 0;
        codes_handle *h = nullptr;
        size_t msg_index = 0;

        while (true) {
            long long file_offset = static_cast<long long>(ftello(fp));
            h = codes_handle_new_from_file(nullptr, fp, PRODUCT_GRIB, &err);
            if (!h) break;
            MessageMeta m;
            m.global_order = messages.size();
            m.file_index = static_cast<int>(fi);
            m.message_index = msg_index;
            m.file_offset = file_offset;

            m.short_name = get_codes_value_as_string(h, "shortName");
            // Include GRIB1/2 code-table context: distinct unknown parameters
            // must never share a reconstruction template or default variable stream.
            for (const auto* key : {"edition","discipline","parameterCategory","parameterNumber",
                                    "table2Version","indicatorOfParameter","centre","subCentre",
                                    "tablesVersion","localTablesVersion"}) {
                const auto value = get_codes_value_as_string(h, key);
                m.raw_parameter_key += std::string(key)+"="+std::to_string(value.size())+":"+value+"|";
            }
            m.type_of_level = get_codes_value_as_string(h, "typeOfLevel");
            m.level = get_codes_value_as_string(h, "level");
            m.ensemble_number = get_codes_value_as_string(h, "number");
            m.perturbation_number = get_codes_value_as_string(h, "perturbationNumber");
            m.data_date = get_codes_value_as_string(h, "dataDate");
            m.data_time = get_codes_value_as_string(h, "dataTime");
            m.validity_date = get_codes_value_as_string(h, "validityDate");
            m.validity_time = get_codes_value_as_string(h, "validityTime");
            m.step_range = get_codes_value_as_string(h, "stepRange");
            m.timestamp = make_iso_timestamp(
                m.data_date,
                m.data_time,
                m.step_range,
                m.validity_date,
                m.validity_time);
            m.recipe_index=recipes.match(h);
            if(m.recipe_index>=0&&!recipe_settings.count(m.recipe_index))recipe_settings.emplace(m.recipe_index,recipe_options(opts,recipes.rules.at(size_t(m.recipe_index))));
            const Options& field_opts=m.recipe_index<0?opts:recipe_settings.at(m.recipe_index);
            m.encoding_options=field_opts;
            // Prevent unlike statistics/initializations from sharing recipe streams.
            for(const auto* key:{"stepType","stepUnits","dataDate","dataTime","typeOfEnsembleForecast","percentileValue","probabilityType"})
                m.recipe_scope+=get_codes_value_as_string(h,key)+"|";
            m.apply_log2 = field_opts.transform == "log";
            m.apply_asinh = field_opts.transform == "asinh";
            m.log_offset = 1.0;
            m.apply_shifted_log=field_opts.transform=="shifted-log";
            m.transform_scale=m.apply_shifted_log?field_opts.log_alpha:field_opts.asinh_scale;

            long lval = 0;
            double dval = 0.0;

            if (codes_get_long(h, "Ni", &lval) == 0) m.nx = static_cast<int>(lval);
            if (codes_get_long(h, "Nj", &lval) == 0) m.ny = static_cast<int>(lval);

            if (codes_get_double(h, "latitudeOfFirstGridPointInDegrees", &dval) == 0) m.lat_first = dval;
            if (codes_get_double(h, "latitudeOfLastGridPointInDegrees", &dval) == 0) m.lat_last = dval;
            if (codes_get_double(h, "longitudeOfFirstGridPointInDegrees", &dval) == 0) m.lon_first = dval;
            if (codes_get_double(h, "longitudeOfLastGridPointInDegrees", &dval) == 0) m.lon_last = dval;
            if (codes_get_double(h, "iDirectionIncrementInDegrees", &dval) == 0) m.dx = dval;
            if (codes_get_double(h, "jDirectionIncrementInDegrees", &dval) == 0) m.dy = dval;

            if (codes_get_long(h, "bitmapPresent", &lval) == 0) m.bitmap_present = (lval != 0);
            (void)codes_get_long(h, "scanningMode", &m.scanning_mode);
            std::vector<uint8_t> scan_bitmap;
            if (m.bitmap_present) {
                scan_bitmap = avenc_bitmap::read(h, size_t(m.nx)*m.ny);
                m.bitmap_md5 = avenc_bitmap::md5(scan_bitmap);
            }
            if (codes_get_long(h, "bitsPerValue", &lval) == 0) m.bits_per_value = static_cast<int>(lval);
            if (codes_get_double(h, "missingValue", &dval) == 0) m.missing_value = dval;

            size_t nvals = 0;
            if (!opts.headers_only_scan && !skip_template && codes_get_size(h, "values", &nvals) == 0 && nvals > 0) {
                const std::string tkey = template_cache_key(m, h);
                auto it = template_cache.find(tkey);
                if (it != template_cache.end()) {
                    m.template_b64 = it->second;
                } else {
                    const auto t0 = clock::now();
                    m.template_b64 = make_template_payload_base64(h, nvals);
                    const auto t1 = clock::now();
                    template_sec += std::chrono::duration<double>(t1 - t0).count();
                    template_cache.emplace(tkey, m.template_b64);
                }
            }

            // ecCodes statistics also skip values equal to missingValue. Change
            // that transient sentinel before requesting statistics, otherwise a
            // legitimate 9999 extremum can silently shrink the quantization range.
            if (m.bitmap_present && codes_set_double(h, "missingValue", CODES_MISSING_DOUBLE) != 0)
                throw std::runtime_error("Unable to set GRIB statistics missing-value sentinel");
            bool got_min = (codes_get_double(h, "minimum", &m.value_min) == 0);
            bool got_max = (codes_get_double(h, "maximum", &m.value_max) == 0);
            if (!(got_min && got_max && std::isfinite(m.value_min) && std::isfinite(m.value_max))) {
                ++fallback_minmax_count;
                if (allow_fallback_minmax) {
                    const auto t0 = clock::now();
                    if (codes_get_size(h, "values", &nvals) == 0 && nvals > 0) {
                        std::vector<double> vals(nvals);
                        codes_get_double_array(h, "values", vals.data(), &nvals);
                        double local_min = std::numeric_limits<double>::infinity();
                        double local_max = -std::numeric_limits<double>::infinity();
                        for (size_t i=0; i<vals.size(); ++i) {
                            const double v=vals[i];
                            if (field_missing(v, scan_bitmap, i)) continue;
                            if (v < local_min) local_min = v;
                            if (v > local_max) local_max = v;
                        }
                        if (std::isfinite(local_min) && std::isfinite(local_max)) {
                            m.value_min = local_min;
                            m.value_max = local_max;
                        }
                    }
                    const auto t1 = clock::now();
                    fallback_minmax_sec += std::chrono::duration<double>(t1 - t0).count();
                }
            }

            if (std::isfinite(m.value_min) && std::isfinite(m.value_max)) {
                m.quant_min = m.apply_shifted_log?m.value_min:encode_transform(m.value_min, m.apply_log2, m.apply_asinh, m.log_offset,false,m.transform_scale);
                m.quant_max = m.apply_shifted_log?m.value_max:encode_transform(m.value_max, m.apply_log2, m.apply_asinh, m.log_offset,false,m.transform_scale);
            }

            if (std::isfinite(m.quant_min) && m.quant_min < global_min) global_min = m.quant_min;
            if (std::isfinite(m.quant_max) && m.quant_max > global_max) global_max = m.quant_max;

            messages.push_back(std::move(m));
            codes_handle_delete(h);
            ++msg_index;
            if (progress_cb) {
                long long pos = ftello(fp);
                uint64_t local_done = (pos > 0) ? static_cast<uint64_t>(pos) : 0u;
                uint64_t done = bytes_done + std::min(local_done, file_bytes);
                progress_cb(done, total_input_bytes);
            }
        }

        if (err != 0 && err != GRIB_END_OF_FILE) {
            std::fclose(fp);
            throw std::runtime_error("Error while scanning " + path + ": " + codes_get_error_message(err));
        }
        std::fclose(fp);
        bytes_done += file_bytes;
        if (progress_cb) progress_cb(bytes_done, total_input_bytes);
    }

    if (!std::isfinite(global_min) || !std::isfinite(global_max) || global_max <= global_min) {
        // Keep scanning/header-sorting robust even when min/max keys are absent.
        global_min = 0.0;
        global_max = 1.0;
    }

    if (opts.verbose) {
        const auto scan_t1 = clock::now();
        const double scan_sec = std::chrono::duration<double>(scan_t1 - scan_t0).count();
        std::cerr << "[profile] scan_total_sec=" << scan_sec
                  << " template_sec=" << template_sec
                  << " fallback_minmax_sec=" << fallback_minmax_sec
                  << " fallback_minmax_count=" << fallback_minmax_count
                  << " template_cache_size=" << template_cache.size()
                  << " allow_fallback_minmax=" << (allow_fallback_minmax ? 1 : 0)
                  << " skip_template=" << (skip_template ? 1 : 0)
                  << " headers_only_scan=" << (opts.headers_only_scan ? 1 : 0)
                  << "\n";
    }
    if (profile) {
        profile->template_sec = template_sec;
        profile->fallback_minmax_sec = fallback_minmax_sec;
        profile->fallback_minmax_count = fallback_minmax_count;
        profile->template_cache_size = template_cache.size();
    }
}

std::vector<Group> build_groups(const std::vector<MessageMeta> &messages, const std::string &mode) {
    std::map<std::string, Group> group_map;

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto &m = messages[i];
        const std::string effective_mode=m.recipe_index<0||m.encoding_options.group_by=="auto"?mode:m.encoding_options.group_by;
        std::string key = grouping_key(m, effective_mode);
        if(m.recipe_index>=0)key+="|recipe="+std::to_string(m.recipe_index)+"|"+m.recipe_scope;

        auto &g = group_map[key];
        if (g.key.empty()) {
            g.key = key;
            g.label = group_label(m, effective_mode);
            g.encoding_options=m.encoding_options;
            g.short_name = m.short_name;
            g.type_of_level = m.type_of_level;
            g.level = (effective_mode == "var") ? "" : m.level;
            g.nx = m.nx;
            g.ny = m.ny;
            g.lat_first = m.lat_first;
            g.lat_last = m.lat_last;
            g.lon_first = m.lon_first;
            g.lon_last = m.lon_last;
            g.dx = m.dx;
            g.dy = m.dy;
            g.apply_log2 = m.apply_log2;
            g.apply_asinh = m.apply_asinh;
            g.log_offset = m.log_offset;
            g.apply_shifted_log=m.apply_shifted_log;g.transform_scale=m.transform_scale;
        }

        if(std::isfinite(m.value_min))g.raw_min=std::min(g.raw_min,m.value_min);
        if(std::isfinite(m.value_max))g.raw_max=std::max(g.raw_max,m.value_max);
        g.message_indices.push_back(i);
        g.apply_log2 = g.apply_log2 || m.apply_log2;
        g.apply_asinh = g.apply_asinh || m.apply_asinh;
        if (std::isfinite(m.quant_min) && m.quant_min < g.value_min) g.value_min = m.quant_min;
        if (std::isfinite(m.quant_max) && m.quant_max > g.value_max) g.value_max = m.quant_max;
    }

    std::vector<Group> groups;
    groups.reserve(group_map.size());

    for (auto &kv : group_map) {
        Group g = std::move(kv.second);
        g.message_indices_sorted = g.message_indices;
        std::sort(g.message_indices_sorted.begin(), g.message_indices_sorted.end(),
                  [&](size_t a, size_t b) {
                      const auto &ma = messages[a];
                      const auto &mb = messages[b];
                      if (ma.timestamp == mb.timestamp) {
                          if (ma.file_index == mb.file_index) return ma.message_index < mb.message_index;
                          return ma.file_index < mb.file_index;
                      }
                      return ma.timestamp < mb.timestamp;
                  });
        g.frame_count = g.message_indices_sorted.size();
        if(g.apply_shifted_log){
            g.transform_origin=std::isfinite(g.raw_min)?g.raw_min:0;
            g.transform_span=g.raw_max>g.raw_min?g.raw_max-g.raw_min:1;
            avenc_transform::validate(g.transform_scale,g.transform_origin,g.transform_span);
            g.value_min=0;g.value_max=g.raw_max>g.raw_min?1:0;
        }
        if (std::isfinite(g.value_min) && std::isfinite(g.value_max) && g.value_max <= g.value_min) {
            const double eps = std::max(1e-9, std::fabs(g.value_min) * 1e-9);
            g.value_max = g.value_min + eps;
        }
        groups.push_back(std::move(g));
    }

    std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        if (a.short_name == b.short_name) {
            if (a.type_of_level == b.type_of_level) return a.level < b.level;
            return a.type_of_level < b.type_of_level;
        }
        return a.short_name < b.short_name;
    });

    size_t frame_start = 0;
    for (auto &g : groups) {
        g.frame_start = frame_start;
        frame_start += g.frame_count;
    }

    return groups;
}

bool group_varies_by_level(const Group &g, const std::vector<MessageMeta> &messages) {
    if (g.message_indices_sorted.empty()) return false;
    const auto &m0 = messages[g.message_indices_sorted.front()];
    const std::string first_level = m0.type_of_level + "|" + m0.level;
    for (size_t idx : g.message_indices_sorted) {
        const auto &m = messages[idx];
        if (m.type_of_level + "|" + m.level != first_level) return true;
    }
    return false;
}

void prepare_shared_bitmaps(const Options& opts, const std::vector<MessageMeta>& messages,
                            std::vector<Group>& groups) {
    for (auto& g:groups) {
        const auto& first=messages[g.message_indices.front()];
        const bool shared=first.bitmap_present && std::all_of(
            g.message_indices.begin(),g.message_indices.end(),[&](size_t i) {
                const auto& m=messages[i];
                return m.bitmap_present && m.bitmap_md5==first.bitmap_md5 &&
                    m.nx==first.nx && m.ny==first.ny && m.scanning_mode==first.scanning_mode &&
                    m.lat_first==first.lat_first && m.lat_last==first.lat_last &&
                    m.lon_first==first.lon_first && m.lon_last==first.lon_last &&
                    m.dx==first.dx && m.dy==first.dy;
            });
        if (shared) {
            FILE* fp=std::fopen(opts.inputs[first.file_index].c_str(),"rb");
            if (!fp) throw std::runtime_error("Cannot reopen shared bitmap source");
            if (fseeko(fp,static_cast<off_t>(first.file_offset),SEEK_SET)!=0) {
                std::fclose(fp); throw std::runtime_error("Cannot seek shared bitmap source");
            }
            int err=0;
            codes_handle* h=codes_handle_new_from_file(nullptr,fp,PRODUCT_GRIB,&err);
            std::fclose(fp);
            if (!h || err) { if(h) codes_handle_delete(h); throw std::runtime_error("Cannot read shared bitmap source"); }
            try { g.shared_bitmap=avenc_bitmap::read(h,size_t(g.nx)*g.ny); }
            catch (...) { codes_handle_delete(h);throw; }
            codes_handle_delete(h);
            g.bitmap_md5=avenc_bitmap::md5(g.shared_bitmap);
            if (g.bitmap_md5!=first.bitmap_md5)
                throw std::runtime_error("Source bitmap changed since initial scan");
        }
        if (opts.verbose) std::cout<<"[bitmap] group="<<g.label<<" shared="<<shared
            <<" md5="<<(shared?g.bitmap_md5:"none")<<" mask-fill="
            <<(shared?opts.mask_fill:"min (no shared bitmap)")<<"\n";
        if (!shared && opts.mask_fill=="taper3" && first.bitmap_present)
            std::cerr<<"[bitmap] "<<g.label<<": changing/incompatible bitmap; taper3 disabled\n";
    }
}

bool use_group_minmax_for_group(const Options &opts,
                                const Group &g,
                                const std::vector<MessageMeta> &messages) {
    if (opts.quantization == "groupMinMax") return true;
    if (opts.quantization == "frameMinMax") return false;
    // auto:
    // - frameMinMax for groups that span multiple levels (e.g. var-grouped vertical profiles),
    // - groupMinMax for single-level time streams to keep luminance scale consistent over time.
    if (group_varies_by_level(g, messages)) return false;
    return true;
}

bool use_adaptive_group_minmax_for_group(const Options &opts,
                                         const Group &g,
                                         const std::vector<MessageMeta> &messages) {
    const bool force_adaptive = (std::getenv("AVPACK_FORCE_ADAPTIVE_MINMAX") != nullptr);
    if (!force_adaptive) return false;
    if (g.frame_count <= 1000) return false;
    if (group_varies_by_level(g, messages)) return false;
    if (opts.quantization == "frameMinMax") return false;
    // Opt-in only via AVPACK_FORCE_ADAPTIVE_MINMAX for experiments.
    return true;
}

std::pair<double, double> quant_range_for_record(const Options &opts,
                                                 const Group &g,
                                                 const MessageMeta &m,
                                                 const std::vector<MessageMeta> &messages) {
    const bool use_group_minmax = use_group_minmax_for_group(opts, g, messages);
    double frame_min = use_group_minmax ? g.value_min : m.quant_min;
    double frame_max = use_group_minmax ? g.value_max : m.quant_max;
    if(g.apply_shifted_log&&!use_group_minmax){
        frame_min=encode_transform(m.value_min,false,false,1,true,g.transform_scale,g.transform_origin,g.transform_span);
        frame_max=encode_transform(m.value_max,false,false,1,true,g.transform_scale,g.transform_origin,g.transform_span);
    }
    if (!std::isfinite(frame_min) || !std::isfinite(frame_max) || frame_max <= frame_min) {
        frame_min = g.value_min;
        frame_max = g.value_max;
    }
    if (!std::isfinite(frame_min) || !std::isfinite(frame_max) || frame_max <= frame_min) {
        throw std::runtime_error("Invalid value range for frame in group: " + g.label);
    }
    if (frame_max <= frame_min) {
        const double eps = std::max(1e-9, std::fabs(frame_min) * 1e-9);
        frame_max = frame_min + eps;
    }
    return {frame_min, frame_max};
}

std::pair<double, double> quant_range_for_assignment(
    const Options &opts,
    const Group &g,
    const MessageMeta &m,
    const std::vector<MessageMeta> &messages,
    const GroupRuntimeInfo *ri,
    size_t frame_index_in_group) {
    if (ri &&
        frame_index_in_group < ri->frame_quant_min.size() &&
        frame_index_in_group < ri->frame_quant_max.size()) {
        const double qmin = ri->frame_quant_min[frame_index_in_group];
        const double qmax = ri->frame_quant_max[frame_index_in_group];
        if (std::isfinite(qmin) && std::isfinite(qmax) && qmax > qmin) {
            return {qmin, qmax};
        }
    }
    return quant_range_for_record(opts, g, m, messages);
}

std::string shell_quote(const std::string &arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

size_t encode_single_group_streaming(const Options &opts,
                                     std::vector<MessageMeta> &messages,
                                     const Group &group,
                                     const std::string &output_video_path,
                                     GroupRuntimeInfo &runtime_info,
                                     std::unordered_map<std::string, std::string> *template_cache,
                                     double *template_deferred_sec,
                                     const std::function<void(uint64_t)> &progress_cb = {}) {
    const size_t pixels = static_cast<size_t>(group.nx) * static_cast<size_t>(group.ny);
    const int storage_shift = storage_shift_for_bits(opts);
    const uint32_t scale = (1u << opts.bits) - 1u;
    const int pad_align = detect_padding_alignment(opts);
    const int enc_nx = align_up_to(group.nx, pad_align);
    const int enc_ny = align_up_to(group.ny, pad_align);
    const size_t enc_pixels = static_cast<size_t>(enc_nx) * static_cast<size_t>(enc_ny);

    runtime_info.apply_log2 = group.apply_log2;
    runtime_info.apply_asinh = group.apply_asinh;
    runtime_info.log_offset = group.log_offset;
    runtime_info.apply_shifted_log=group.apply_shifted_log;runtime_info.transform_scale=group.transform_scale;runtime_info.transform_origin=group.transform_origin;runtime_info.transform_span=group.transform_span;
    runtime_info.shared_bitmap_present = false;
    runtime_info.shared_bitmap_payload.clear();
    runtime_info.adaptive_group_minmax = use_adaptive_group_minmax_for_group(opts, group, messages);
    runtime_info.encoded_nx = enc_nx;
    runtime_info.encoded_ny = enc_ny;
    runtime_info.crop_nx = group.nx;
    runtime_info.crop_ny = group.ny;
    runtime_info.frame_quant_min.assign(group.frame_count, std::numeric_limits<double>::quiet_NaN());
    runtime_info.frame_quant_max.assign(group.frame_count, std::numeric_limits<double>::quiet_NaN());
    runtime_info.frame_shift_bits.assign(group.frame_count, 0);
    bool adaptive_init = false;
    double adaptive_min = std::numeric_limits<double>::infinity();
    double adaptive_max = -std::numeric_limits<double>::infinity();
    size_t fastpath_frames = 0;

    std::ostringstream cmd;
    cmd << "ffmpeg -y -loglevel error"
        << " -f rawvideo -pix_fmt " << avenc_raw_gray_format(opts.bits, storage_shift_for_bits(opts))
        << " -video_size " << enc_nx << "x" << enc_ny
        << " -framerate 1"
        << " -i -";

    for (const auto &kv : encoder_options(opts, enc_nx, enc_ny)) {
        cmd << " " << shell_quote(kv.first) << " " << shell_quote(kv.second);
    }
    for (const auto &tok : opts.ffmpeg_extra_args) {
        cmd << " " << shell_quote(tok);
    }
    const std::string enc_name = ffmpeg_codec_name(opts);
    cmd << " -c:v " << shell_quote(enc_name);
    if (enc_name == "libx265") {
        cmd << " -x265-params " << shell_quote("log-level=error");
    }
    cmd << " -movflags " << shell_quote("+faststart")
        << " -r 1 -f mp4 " << shell_quote(output_video_path);

    FILE *pipe = popen(cmd.str().c_str(), "w");
    if (!pipe) throw std::runtime_error("Failed to launch ffmpeg streaming encoder");

    const auto halo = opts.mask_fill == "taper3" && !group.shared_bitmap.empty()
        ? avenc_bitmap::halo3(group.shared_bitmap,group.nx,group.ny)
        : std::vector<avenc_bitmap::HaloCell>{};
    if (!group.shared_bitmap.empty()) {
        runtime_info.shared_bitmap_present = true;
        runtime_info.shared_bitmap_payload = zlib_base64_payload(
            avenc_bitmap::expand(group.shared_bitmap,pixels));
    }
    std::vector<FILE *> file_handles(opts.inputs.size(), nullptr);
    std::vector<double> values(pixels);
    std::vector<uint8_t> out8;
    std::vector<uint16_t> out16;
    std::vector<uint32_t> q_work;

    auto close_all_inputs = [&]() {
        for (FILE *fh : file_handles) {
            if (fh) std::fclose(fh);
        }
        std::fill(file_handles.begin(), file_handles.end(), nullptr);
    };

    try {
        for (size_t fi = 0; fi < opts.inputs.size(); ++fi) {
            file_handles[fi] = std::fopen(opts.inputs[fi].c_str(), "rb");
            if (!file_handles[fi]) {
                throw std::runtime_error(
                    "Unable to open GRIB file in streaming encode pass: " + opts.inputs[fi]);
            }
        }

        for (size_t frame_idx = 0; frame_idx < group.message_indices_sorted.size(); ++frame_idx) {
            const size_t mi = group.message_indices_sorted[frame_idx];
            auto &m = messages[mi];
            if (m.file_index < 0 || static_cast<size_t>(m.file_index) >= file_handles.size()) {
                throw std::runtime_error("Invalid file index while streaming");
            }
            FILE *fp = file_handles[static_cast<size_t>(m.file_index)];
            if (!fp) throw std::runtime_error("Missing open file handle while streaming");
            if (m.file_offset < 0) throw std::runtime_error("Missing file offset for streaming encode");
            if (fseeko(fp, static_cast<off_t>(m.file_offset), SEEK_SET) != 0) {
                throw std::runtime_error("fseeko failed in streaming encode");
            }

            int err = 0;
            codes_handle *h = codes_handle_new_from_file(nullptr, fp, PRODUCT_GRIB, &err);
            if (!h || err != 0) {
                if (h) codes_handle_delete(h);
                throw std::runtime_error(
                    "Unable to read GRIB message at offset in streaming encode: " +
                    opts.inputs[static_cast<size_t>(m.file_index)]);
            }

            if (opts.headers_only_scan && m.template_b64.empty()) {
                size_t nvals_for_template = 0;
                if (codes_get_size(h, "values", &nvals_for_template) == 0 && nvals_for_template > 0) {
                    const std::string tkey = template_cache_key(m, h);
                    std::string tval;
                    bool found = false;
                    if (template_cache) {
                        auto it = template_cache->find(tkey);
                        if (it != template_cache->end()) {
                            tval = it->second;
                            found = true;
                        }
                    }
                    if (!found) {
                        using clock = std::chrono::steady_clock;
                        const auto t0 = clock::now();
                        tval = make_template_payload_base64(h, nvals_for_template);
                        const auto t1 = clock::now();
                        if (template_deferred_sec) {
                            *template_deferred_sec += std::chrono::duration<double>(t1 - t0).count();
                        }
                        if (template_cache) {
                            (*template_cache)[tkey] = tval;
                        }
                    }
                    m.template_b64 = tval;
                }
            }

            const auto fp_data = try_extract_simple_packed_fastpath(opts, runtime_info, group, h, pixels);
            if (fp_data.ok) {
                runtime_info.frame_quant_min[frame_idx] = fp_data.frame_min;
                runtime_info.frame_quant_max[frame_idx] = fp_data.frame_max;
                runtime_info.frame_shift_bits[frame_idx] = fp_data.shift_bits;

                if (opts.bits <= 8) {
                    pack_with_padding_8bit(fp_data.q_values, group.nx, group.ny, enc_nx, enc_ny, out8);
                    if (std::fwrite(out8.data(), sizeof(uint8_t), enc_pixels, pipe) != enc_pixels) {
                        codes_handle_delete(h);
                        throw std::runtime_error("Failed to stream 8-bit fast-path frame to ffmpeg");
                    }
                } else {
                    pack_with_padding_16bit(
                        fp_data.q_values,
                        group.nx,
                        group.ny,
                        enc_nx,
                        enc_ny,
                        storage_shift,
                        out16);
                    if (std::fwrite(out16.data(), sizeof(uint16_t), enc_pixels, pipe) != enc_pixels) {
                        codes_handle_delete(h);
                        throw std::runtime_error("Failed to stream 16-bit fast-path frame to ffmpeg");
                    }
                }
                ++fastpath_frames;
                if (progress_cb) progress_cb(1);
                codes_handle_delete(h);
                continue;
            }

            size_t nvals = 0;
            if (codes_get_size(h, "values", &nvals) != 0 || nvals != pixels) {
                codes_handle_delete(h);
                throw std::runtime_error("Unexpected values size in streaming encode");
            }
            codes_get_double_array(h, "values", values.data(), &nvals);

            long bitmap_present = 0;
            (void)codes_get_long(h, "bitmapPresent", &bitmap_present);
            const bool has_bitmap = (bitmap_present != 0);
            const auto frame_bitmap = has_bitmap && group.shared_bitmap.empty()
                ? avenc_bitmap::read(h,pixels) : std::vector<uint8_t>{};
            const auto& bitmap = group.shared_bitmap.empty() ? frame_bitmap : group.shared_bitmap;
            const double qmax_d = static_cast<double>(scale);
            const double qvalid_d = has_bitmap ? static_cast<double>(scale - 1u) : qmax_d;
            bool values_pretransformed = false;

            double frame_min = 0.0;
            double frame_max = 0.0;
            if (runtime_info.adaptive_group_minmax) {
                double data_min = std::numeric_limits<double>::infinity();
                double data_max = -std::numeric_limits<double>::infinity();
                for (size_t i = 0; i < pixels; ++i) {
                    const double raw = values[i];
                    if (field_missing(raw, bitmap, i)) continue;
                    const double tv = encode_transform(raw, runtime_info.apply_log2, runtime_info.apply_asinh, runtime_info.log_offset, runtime_info.apply_shifted_log, runtime_info.transform_scale, runtime_info.transform_origin, runtime_info.transform_span);
                    values[i] = tv;
                    if (tv < data_min) data_min = tv;
                    if (tv > data_max) data_max = tv;
                }
                if (!std::isfinite(data_min) || !std::isfinite(data_max)) {
                    auto qr = quant_range_for_record(opts, group, m, messages);
                    data_min = qr.first;
                    data_max = qr.second;
                }
                if (!adaptive_init) {
                    adaptive_min = data_min;
                    adaptive_max = data_max;
                    double span0 = adaptive_max - adaptive_min;
                    if (span0 <= 0.0) span0 = std::max(1e-6, std::fabs(adaptive_min) * 1e-6);
                    adaptive_min -= 0.1 * span0;
                    adaptive_max += 0.1 * span0;
                    adaptive_init = true;
                }
                if (data_min < adaptive_min || data_max > adaptive_max) {
                    adaptive_min = std::min(adaptive_min, data_min);
                    adaptive_max = std::max(adaptive_max, data_max);
                    double span = adaptive_max - adaptive_min;
                    if (span <= 0.0) span = std::max(1e-6, std::fabs(adaptive_min) * 1e-6);
                    adaptive_min -= 0.1 * span;
                    adaptive_max += 0.1 * span;
                }
                frame_min = adaptive_min;
                frame_max = adaptive_max;
                values_pretransformed = true;
            } else {
                auto quant_range = quant_range_for_record(opts, group, m, messages);
                frame_min = quant_range.first;
                frame_max = quant_range.second;
            }
            runtime_info.frame_quant_min[frame_idx] = frame_min;
            runtime_info.frame_quant_max[frame_idx] = frame_max;
            runtime_info.frame_shift_bits[frame_idx] = 0;
            const double frame_span = std::max(1e-12, frame_max - frame_min);
            const double inv_span = 1.0 / frame_span;

            q_work.assign(pixels, 0u);
            if (opts.bits <= 8) {
                for (size_t i = 0; i < pixels; ++i) {
                    double v = values[i];
                    const bool missing = field_missing(v, bitmap, i);
                    uint32_t q = 0u;
                    if (missing && has_bitmap) {
                        q = 0u;
                    } else {
                        if (!values_pretransformed) {
                            v = encode_transform(v, runtime_info.apply_log2, runtime_info.apply_asinh, runtime_info.log_offset, runtime_info.apply_shifted_log, runtime_info.transform_scale, runtime_info.transform_origin, runtime_info.transform_span);
                        }
                        if (v < frame_min) v = frame_min;
                        if (v > frame_max) v = frame_max;
                        double norm = (v - frame_min) * inv_span;
                        if (norm < 0.0) norm = 0.0;
                        if (norm > 1.0) norm = 1.0;
                        if (has_bitmap) {
                            q = 1u + static_cast<uint32_t>(std::llround(norm * qvalid_d));
                            if (q < 1u) q = 1u;
                            if (q > scale) q = scale;
                        } else {
                            q = static_cast<uint32_t>(std::llround(norm * qmax_d));
                            if (q > scale) q = scale;
                        }
                    }
                    q_work[i] = q;
                }
                avenc_bitmap::taper(q_work,halo);
                pack_with_padding_8bit(q_work, group.nx, group.ny, enc_nx, enc_ny, out8);
                if (std::fwrite(out8.data(), sizeof(uint8_t), enc_pixels, pipe) != enc_pixels) {
                    codes_handle_delete(h);
                    throw std::runtime_error("Failed to stream 8-bit frame to ffmpeg");
                }
            } else {
                for (size_t i = 0; i < pixels; ++i) {
                    double v = values[i];
                    const bool missing = field_missing(v, bitmap, i);
                    uint32_t q = 0u;
                    if (missing && has_bitmap) {
                        q = 0u;
                    } else {
                        if (!values_pretransformed) {
                            v = encode_transform(v, runtime_info.apply_log2, runtime_info.apply_asinh, runtime_info.log_offset, runtime_info.apply_shifted_log, runtime_info.transform_scale, runtime_info.transform_origin, runtime_info.transform_span);
                        }
                        if (v < frame_min) v = frame_min;
                        if (v > frame_max) v = frame_max;
                        double norm = (v - frame_min) * inv_span;
                        if (norm < 0.0) norm = 0.0;
                        if (norm > 1.0) norm = 1.0;
                        if (has_bitmap) {
                            q = 1u + static_cast<uint32_t>(std::llround(norm * qvalid_d));
                            if (q < 1u) q = 1u;
                            if (q > scale) q = scale;
                        } else {
                            q = static_cast<uint32_t>(std::llround(norm * qmax_d));
                            if (q > scale) q = scale;
                        }
                    }
                    q_work[i] = q;
                }
                avenc_bitmap::taper(q_work,halo);
                pack_with_padding_16bit(q_work, group.nx, group.ny, enc_nx, enc_ny, storage_shift, out16);
                if (std::fwrite(out16.data(), sizeof(uint16_t), enc_pixels, pipe) != enc_pixels) {
                    codes_handle_delete(h);
                    throw std::runtime_error("Failed to stream 16-bit frame to ffmpeg");
                }
            }

            if (!opts.quiet && opts.verbose &&
                (frame_idx == 0 ||
                 (frame_idx % 100) == 0 ||
                 (frame_idx + 1) == group.message_indices_sorted.size())) {
                std::cout << "[encode-stream] " << opts.inputs[static_cast<size_t>(m.file_index)]
                          << " msg=" << m.message_index
                          << " -> group=" << group.label << " frame=" << frame_idx << "\n";
            }

            if (progress_cb) progress_cb(1);
            codes_handle_delete(h);
        }
        close_all_inputs();
    } catch (...) {
        close_all_inputs();
        int ret = pclose(pipe);
        (void)ret;
        throw;
    }

    int ret = pclose(pipe);
    if (ret != 0) {
        throw std::runtime_error("ffmpeg streaming encoder failed with code " + std::to_string(ret));
    }
    if (opts.verbose && fastpath_frames > 0) {
        std::cout << "[fastpath] group=" << group.label
                  << " frames=" << fastpath_frames << "/" << group.frame_count
                  << " encoded_size=" << enc_nx << "x" << enc_ny
                  << " crop=" << group.nx << "x" << group.ny
                  << "\n";
    }
    return fastpath_frames;
}

void write_group_raw_files(const Options &opts,
                           const std::vector<MessageMeta> &messages,
                           const std::vector<Group> &groups,
                           std::vector<std::string> &group_raw_paths,
                           std::vector<GroupRuntimeInfo> &runtime_infos,
                           const std::function<void(uint64_t)> &progress_cb = {}) {
    if (groups.empty()) throw std::runtime_error("No groups to encode");
    const int nx0 = groups.front().nx;
    const int ny0 = groups.front().ny;
    for (const auto &g : groups) {
        if (g.nx != nx0 || g.ny != ny0) {
            throw std::runtime_error("All groups must share the same grid dimensions");
        }
    }

    const size_t bytes_per_pixel = (opts.bits <= 8) ? sizeof(uint8_t) : sizeof(uint16_t);
    const int storage_shift = storage_shift_for_bits(opts);
    runtime_infos.assign(groups.size(), GroupRuntimeInfo{});
    for (size_t gid = 0; gid < groups.size(); ++gid) {
        runtime_infos[gid].apply_log2 = groups[gid].apply_log2;
        runtime_infos[gid].apply_asinh = groups[gid].apply_asinh;
        runtime_infos[gid].log_offset = groups[gid].log_offset;
    runtime_infos[gid].apply_shifted_log=groups[gid].apply_shifted_log;runtime_infos[gid].transform_scale=groups[gid].transform_scale;runtime_infos[gid].transform_origin=groups[gid].transform_origin;runtime_infos[gid].transform_span=groups[gid].transform_span;
        runtime_infos[gid].adaptive_group_minmax =
            use_adaptive_group_minmax_for_group(opts, groups[gid], messages);
        runtime_infos[gid].encoded_nx = groups[gid].nx;
        runtime_infos[gid].encoded_ny = groups[gid].ny;
        runtime_infos[gid].crop_nx = groups[gid].nx;
        runtime_infos[gid].crop_ny = groups[gid].ny;
        runtime_infos[gid].frame_quant_min.assign(groups[gid].frame_count, std::numeric_limits<double>::quiet_NaN());
        runtime_infos[gid].frame_quant_max.assign(groups[gid].frame_count, std::numeric_limits<double>::quiet_NaN());
        runtime_infos[gid].frame_shift_bits.assign(groups[gid].frame_count, 0);
    }

    std::vector<std::unordered_map<size_t, Assignment>> lookup(opts.inputs.size());

    for (size_t gid = 0; gid < groups.size(); ++gid) {
        const auto &g = groups[gid];
        for (size_t frame_idx = 0; frame_idx < g.message_indices_sorted.size(); ++frame_idx) {
            size_t mi = g.message_indices_sorted[frame_idx];
            const auto &m = messages[mi];
            lookup[static_cast<size_t>(m.file_index)][m.message_index] =
                Assignment{static_cast<int>(gid), frame_idx};
        }
    }

    group_raw_paths.clear();
    group_raw_paths.resize(groups.size());
    std::vector<FILE *> group_files(groups.size(), nullptr);

    for (size_t gid = 0; gid < groups.size(); ++gid) {
        char tmp_template[] = "/tmp/avpack_group_rawXXXXXX";
        int fd = mkstemp(tmp_template);
        if (fd == -1) throw std::runtime_error("Unable to create temporary group raw file");
        FILE *fp = fdopen(fd, "w+b");
        if (!fp) {
            close(fd);
            std::remove(tmp_template);
            throw std::runtime_error("Unable to open temporary group raw stream");
        }
        group_raw_paths[gid] = tmp_template;
        group_files[gid] = fp;
    }

    const double scale = static_cast<double>((1u << opts.bits) - 1u);
    std::vector<std::vector<avenc_bitmap::HaloCell>> halos(groups.size());
    for (size_t gid=0;gid<groups.size();++gid) {
        const auto& g=groups[gid];
        if (!g.shared_bitmap.empty()) {
            runtime_infos[gid].shared_bitmap_present=true;
            runtime_infos[gid].shared_bitmap_payload=zlib_base64_payload(
                avenc_bitmap::expand(g.shared_bitmap,size_t(g.nx)*g.ny));
            if (opts.mask_fill=="taper3") halos[gid]=avenc_bitmap::halo3(g.shared_bitmap,g.nx,g.ny);
        }
    }
    std::vector<bool> adaptive_initialized(groups.size(), false);
    std::vector<double> adaptive_min(groups.size(), std::numeric_limits<double>::infinity());
    std::vector<double> adaptive_max(groups.size(), -std::numeric_limits<double>::infinity());

    for (size_t fi = 0; fi < opts.inputs.size(); ++fi) {
        const std::string &path = opts.inputs[fi];
        FILE *fp = std::fopen(path.c_str(), "rb");
        if (!fp) throw std::runtime_error("Unable to open GRIB file in encode pass: " + path);

        int err = 0;
        codes_handle *h = nullptr;
        size_t msg_index = 0;

        while ((h = codes_handle_new_from_file(nullptr, fp, PRODUCT_GRIB, &err)) != nullptr) {
            auto it = lookup[fi].find(msg_index);
            if (it == lookup[fi].end()) {
                codes_handle_delete(h);
                ++msg_index;
                continue;
            }

            const Assignment asg = it->second;
            const Group &g = groups[static_cast<size_t>(asg.group_id)];
            FILE *gfp = group_files[static_cast<size_t>(asg.group_id)];
            const size_t pixels = static_cast<size_t>(g.nx) * static_cast<size_t>(g.ny);
            const size_t frame_bytes = pixels * bytes_per_pixel;
            const size_t mi = g.message_indices_sorted[asg.frame_index_in_group];
            const MessageMeta &fm = messages[mi];
            const auto frame_bitmap=fm.bitmap_present && g.shared_bitmap.empty()
                ? avenc_bitmap::read(h,pixels) : std::vector<uint8_t>{};
            const auto& bitmap=g.shared_bitmap.empty()?frame_bitmap:g.shared_bitmap;

            size_t nvals = 0;
            if (codes_get_size(h, "values", &nvals) != 0 || nvals != pixels) {
                codes_handle_delete(h);
                std::fclose(fp);
                throw std::runtime_error("Unexpected values size in message while encoding");
            }

            std::vector<double> values(nvals);
            codes_get_double_array(h, "values", values.data(), &nvals);

            long bitmap_present = 0;
            codes_get_long(h, "bitmapPresent", &bitmap_present);

            GroupRuntimeInfo &ri = runtime_infos[static_cast<size_t>(asg.group_id)];
            const size_t gid = static_cast<size_t>(asg.group_id);
            bool values_pretransformed = false;

            double frame_min = 0.0;
            double frame_max = 0.0;
            if (ri.adaptive_group_minmax) {
                double data_min = std::numeric_limits<double>::infinity();
                double data_max = -std::numeric_limits<double>::infinity();
                for (size_t i = 0; i < pixels; ++i) {
                    const double raw = values[i];
                    if (field_missing(raw, bitmap, i)) continue;
                    const double tv = encode_transform(raw, ri.apply_log2, ri.apply_asinh, ri.log_offset, ri.apply_shifted_log, ri.transform_scale, ri.transform_origin, ri.transform_span);
                    values[i] = tv;
                    if (tv < data_min) data_min = tv;
                    if (tv > data_max) data_max = tv;
                }
                if (!std::isfinite(data_min) || !std::isfinite(data_max)) {
                    auto qr = quant_range_for_record(opts, g, fm, messages);
                    data_min = qr.first;
                    data_max = qr.second;
                }
                if (!adaptive_initialized[gid]) {
                    adaptive_min[gid] = data_min;
                    adaptive_max[gid] = data_max;
                    double span0 = adaptive_max[gid] - adaptive_min[gid];
                    if (span0 <= 0.0) span0 = std::max(1e-6, std::fabs(adaptive_min[gid]) * 1e-6);
                    adaptive_min[gid] -= 0.1 * span0;
                    adaptive_max[gid] += 0.1 * span0;
                    adaptive_initialized[gid] = true;
                }
                if (data_min < adaptive_min[gid] || data_max > adaptive_max[gid]) {
                    adaptive_min[gid] = std::min(adaptive_min[gid], data_min);
                    adaptive_max[gid] = std::max(adaptive_max[gid], data_max);
                    double span = adaptive_max[gid] - adaptive_min[gid];
                    if (span <= 0.0) span = std::max(1e-6, std::fabs(adaptive_min[gid]) * 1e-6);
                    adaptive_min[gid] -= 0.1 * span;
                    adaptive_max[gid] += 0.1 * span;
                }
                frame_min = adaptive_min[gid];
                frame_max = adaptive_max[gid];
                values_pretransformed = true;
            } else {
                auto quant_range = quant_range_for_record(opts, g, fm, messages);
                frame_min = quant_range.first;
                frame_max = quant_range.second;
            }
            double frame_span = frame_max - frame_min;
            if (frame_span <= 0.0) {
                frame_span = std::max(1e-9, std::fabs(frame_min) * 1e-9);
                frame_max = frame_min + frame_span;
            }
            ri.frame_quant_min[asg.frame_index_in_group] = frame_min;
            ri.frame_quant_max[asg.frame_index_in_group] = frame_max;
            const bool has_bitmap = (bitmap_present != 0);
            const double inv_span = 1.0 / frame_span;
            const double qmax_d = static_cast<double>(scale);
            const double qvalid_d = has_bitmap ? static_cast<double>(scale - 1u) : qmax_d;

            off_t off = static_cast<off_t>(asg.frame_index_in_group) * static_cast<off_t>(frame_bytes);
            if (fseeko(gfp, off, SEEK_SET) != 0) {
                codes_handle_delete(h);
                std::fclose(fp);
                throw std::runtime_error("fseeko failed while writing group frame");
            }

            if (opts.bits <= 8) {
                std::vector<uint8_t> out(pixels);
                for (size_t i = 0; i < pixels; ++i) {
                    double v = values[i];
                    const bool missing = field_missing(v, bitmap, i);
                    uint32_t q = 0u;
                    if (missing && has_bitmap) {
                        q = 0u;
                    } else {
                        if (!values_pretransformed) {
                            v = encode_transform(v, ri.apply_log2, ri.apply_asinh, ri.log_offset, ri.apply_shifted_log, ri.transform_scale, ri.transform_origin, ri.transform_span);
                        }
                        if (v < frame_min) v = frame_min;
                        if (v > frame_max) v = frame_max;
                        double norm = (v - frame_min) * inv_span;
                        if (norm < 0.0) norm = 0.0;
                        if (norm > 1.0) norm = 1.0;
                        if (has_bitmap) {
                            q = 1u + static_cast<uint32_t>(std::llround(norm * qvalid_d));
                            if (q < 1u) q = 1u;
                            if (q > scale) q = scale;
                        } else {
                            q = static_cast<uint32_t>(std::llround(norm * qmax_d));
                            if (q > scale) q = scale;
                        }
                    }
                    out[i] = static_cast<uint8_t>(q);
                }
                avenc_bitmap::taper(out,halos[gid]);
                if (fwrite(out.data(), sizeof(uint8_t), pixels, gfp) != pixels) {
                    codes_handle_delete(h);
                    std::fclose(fp);
                    throw std::runtime_error("Failed to write 8-bit group frame");
                }
            } else {
                std::vector<uint16_t> out(pixels);
                for (size_t i = 0; i < pixels; ++i) {
                    double v = values[i];
                    const bool missing = field_missing(v, bitmap, i);
                    uint32_t q = 0u;
                    if (missing && has_bitmap) {
                        q = 0u;
                    } else {
                        if (!values_pretransformed) {
                            v = encode_transform(v, ri.apply_log2, ri.apply_asinh, ri.log_offset, ri.apply_shifted_log, ri.transform_scale, ri.transform_origin, ri.transform_span);
                        }
                        if (v < frame_min) v = frame_min;
                        if (v > frame_max) v = frame_max;
                        double norm = (v - frame_min) * inv_span;
                        if (norm < 0.0) norm = 0.0;
                        if (norm > 1.0) norm = 1.0;
                        if (has_bitmap) {
                            q = 1u + static_cast<uint32_t>(std::llround(norm * qvalid_d));
                            if (q < 1u) q = 1u;
                            if (q > scale) q = scale;
                        } else {
                            q = static_cast<uint32_t>(std::llround(norm * qmax_d));
                            if (q > scale) q = scale;
                        }
                    }
                    if (storage_shift > 0 && q > 0u) {
                        q = std::min<uint32_t>(65535u, q << storage_shift);
                    }
                    out[i] = static_cast<uint16_t>(q);
                }
                avenc_bitmap::taper(out,halos[gid]);
                if (fwrite(out.data(), sizeof(uint16_t), pixels, gfp) != pixels) {
                    codes_handle_delete(h);
                    std::fclose(fp);
                    throw std::runtime_error("Failed to write 16-bit group frame");
                }
            }

            if (!opts.quiet && opts.verbose &&
                (asg.frame_index_in_group == 0 ||
                 (asg.frame_index_in_group % 100) == 0 ||
                 (asg.frame_index_in_group + 1) == g.frame_count)) {
                std::cout << "[encode] " << path << " msg=" << msg_index
                          << " -> group=" << g.label << " frame=" << asg.frame_index_in_group
                          << "\n";
            }

            if (progress_cb) progress_cb(1);
            codes_handle_delete(h);
            ++msg_index;
        }

        if (err != 0 && err != GRIB_END_OF_FILE) {
            std::fclose(fp);
            throw std::runtime_error("Error in encode pass on " + path + ": " + codes_get_error_message(err));
        }

        std::fclose(fp);
    }

    for (FILE *fp : group_files) {
        if (fp) std::fclose(fp);
    }
}

std::string build_metadata_json(const Options &opts,
                                const std::vector<std::string> &input_files,
                                const std::vector<MessageMeta> &messages,
                                const std::vector<Group> &groups,
                                const std::vector<GroupRuntimeInfo> &runtime_infos) {
    if (groups.empty()) throw std::runtime_error("No groups for metadata");
    if (runtime_infos.size() != groups.size()) {
        throw std::runtime_error("runtime metadata size mismatch");
    }
    const int nx = groups.front().nx;
    const int ny = groups.front().ny;

    auto latitudes = build_latitudes(groups.front());
    auto longitudes = build_longitudes(groups.front());

    std::ostringstream meta;
    const int storage_shift = storage_shift_for_bits(opts);

    meta << "{\"schema\":\"avenc.pack.v1\"";
    size_t total_frames = 0;
    for (const auto &g : groups) total_frames += g.frame_count;

    meta << ",\"frames\":" << total_frames
         << ",\"height\":" << ny
         << ",\"width\":" << nx
         << ",\"shape\":[" << total_frames << "," << ny << "," << nx << "]"
         << ",\"dtype\":\"float32\"";

    double global_min = std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();
    for (const auto &g : groups) {
        if (std::isfinite(g.value_min)) global_min = std::min(global_min, g.value_min);
        if (std::isfinite(g.value_max)) global_max = std::max(global_max, g.value_max);
    }
    if (!std::isfinite(global_min) || !std::isfinite(global_max) || global_max <= global_min) {
        global_min = 0.0;
        global_max = 1.0;
    }
    bool any_log2 = false;
    for (const auto &ri : runtime_infos) any_log2 = any_log2 || ri.apply_log2;

    meta << ",\"grouping\":{\"strategy\":\"" << json_escape(opts.group_by)
         << "\",\"group_count\":" << groups.size()
         << ",\"stream_layout\":\"multi-video-stream\"}";

    const std::string quantization_mode =
        (opts.quantization == "auto") ? "mixedAuto" : opts.quantization;

    meta << ",\"options\":{"
         << "\"bits\":" << opts.bits
         << ",\"min_value\":" << global_min
         << ",\"max_value\":" << global_max
         << ",\"storage_shift\":" << storage_shift
         << ",\"quantization\":\"" << quantization_mode << "\""
         << ",\"transform\":\"" << opts.transform << "\""
         << ",\"chroma\":\"" << json_escape(opts.chroma) << "\""
         << ",\"apply_log2\":" << (any_log2 ? "true" : "false")
         << ",\"allow_nan\":true"
         << ",\"frame_rate\":1"
         << ",\"flip_ud\":false"
         << ",\"codec\":\"" << json_escape(opts.codec) << "\""
         << ",\"quality\":\"" << json_escape(opts.quality) << "\"";
    if (!opts.encoder.empty()) {
        meta << ",\"encoder\":\"" << json_escape(opts.encoder) << "\"";
    }
    meta << "}";

    std::vector<Assignment> msg_to_assignment(messages.size(), Assignment{-1, 0});
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        for (size_t fi = 0; fi < g.message_indices_sorted.size(); ++fi) {
            size_t mi = g.message_indices_sorted[fi];
            msg_to_assignment[mi] = Assignment{static_cast<int>(gi), fi};
        }
    }

    std::ostringstream recon;
    recon << "AVPACK_RECON_V1\n";
    recon << "GRID\t" << nx << "\t" << ny << "\n";
    std::vector<std::string> stream_templates(groups.size(), "-");
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        const auto &ri = runtime_infos[gi];
        const std::string bitmap_payload = ri.shared_bitmap_present ? ri.shared_bitmap_payload : "-";
        std::string stream_template = "-";
        if (!g.message_indices_sorted.empty()) {
            const auto &m0 = messages[g.message_indices_sorted.front()];
            if (!m0.template_b64.empty()) stream_template = m0.template_b64;
        }
        stream_templates[gi] = stream_template;
        recon << "STREAM\t" << gi << "\t" << g.frame_count << "\t" << opts.bits << "\t"
              << std::setprecision(17) << g.value_min << "\t" << g.value_max
              << "\t" << storage_shift
              << "\t" << (ri.apply_shifted_log ? "shifted-log" : ri.apply_asinh ? "asinh" : ri.apply_log2 ? "log2p1" : "linear")
              << "\t" << ri.log_offset
              << "\t" << bitmap_payload
              << "\t" << stream_template
              << "\t" << ri.encoded_nx
              << "\t" << ri.encoded_ny
              << "\t" << ri.crop_nx
              << "\t" << ri.crop_ny
              << "\t" << ri.transform_scale << "\t" << ri.transform_origin << "\t" << ri.transform_span
              << "\n";
    }
    for (size_t mi = 0; mi < messages.size(); ++mi) {
        const auto &m = messages[mi];
        const auto &asg = msg_to_assignment[mi];
        if (asg.group_id < 0) continue;
        const size_t gi = static_cast<size_t>(asg.group_id);
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        const auto &ri = runtime_infos[gi];
        auto quant_range = quant_range_for_assignment(
            opts, g, m, messages, &ri, asg.frame_index_in_group);
        const double record_min = quant_range.first;
        const double record_max = quant_range.second;
        const int shift_bits = (asg.frame_index_in_group < ri.frame_shift_bits.size())
                                   ? ri.frame_shift_bits[asg.frame_index_in_group]
                                   : 0;
        std::string record_template = "-";
        if (gi < stream_templates.size()) {
            if (m.template_b64 != stream_templates[gi]) record_template = m.template_b64;
        } else {
            record_template = m.template_b64;
        }
        recon << "RECORD\t" << m.global_order << "\t" << asg.group_id << "\t" << asg.frame_index_in_group
              << "\t" << m.file_index << "\t" << m.message_index
              << "\t" << tsv_escape(m.short_name)
              << "\t" << tsv_escape(m.type_of_level)
              << "\t" << tsv_escape(m.level)
              << "\t" << tsv_escape(m.data_date)
              << "\t" << tsv_escape(m.data_time)
              << "\t" << tsv_escape(m.step_range)
              << "\t" << tsv_escape(m.timestamp)
              << "\t" << tsv_escape(m.ensemble_number)
              << "\t" << tsv_escape(m.perturbation_number)
              << "\t" << std::setprecision(17) << record_min
              << "\t" << record_max
              << "\t" << (m.bitmap_present ? "1" : "0")
              << "\t" << record_template
              << "\t" << m.bits_per_value
              << "\t" << m.value_min
              << "\t" << m.value_max
              << "\t" << shift_bits
              << "\n";
    }
    const std::string recon_text = recon.str();
    const std::vector<uint8_t> recon_bytes(recon_text.begin(), recon_text.end());
    const std::string recon_payload = zlib_base64_payload(recon_bytes);
    meta << ",\"reconstruct_format\":\"avpack.recon.tsv.zlib.base64.v1\"";
    meta << ",\"reconstruct_payload\":\"" << recon_payload << "\"";

    meta << ",\"source_files\":[";
    for (size_t i = 0; i < input_files.size(); ++i) {
        if (i) meta << ',';
        meta << '"' << json_escape(input_files[i]) << '"';
    }
    meta << "]";

    meta << ",\"coords\":{\"lat\":{\"data\":" << encode_float_array_payload(latitudes)
         << ",\"attrs\":{\"units\":\"degrees_north\"}},"
            "\"lon\":{\"data\":" << encode_float_array_payload(longitudes)
         << ",\"attrs\":{\"units\":\"degrees_east\"}}}";

    meta << ",\"groups\":[";
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        const auto &ri = runtime_infos[gi];
        if (gi) meta << ',';
        const bool use_group_minmax = use_group_minmax_for_group(opts, g, messages);
        std::string group_quant_mode = use_group_minmax ? "groupMinMax" : "frameMinMax";
        if (ri.adaptive_group_minmax) group_quant_mode = "adaptiveGroupMinMax";
        const std::string group_chroma = effective_chroma_for_group(opts, g.nx, g.ny);

        std::vector<std::string> times;
        times.reserve(g.message_indices_sorted.size());
        std::vector<std::string> levels;
        levels.reserve(g.message_indices_sorted.size());
        std::map<std::string, size_t> level_to_index;
        std::map<std::string, size_t> time_to_index;
        for (size_t idx : g.message_indices_sorted) {
            times.push_back(messages[idx].timestamp);
            levels.push_back(messages[idx].level);
            if (level_to_index.find(messages[idx].level) == level_to_index.end()) {
                level_to_index[messages[idx].level] = level_to_index.size();
            }
            if (time_to_index.find(messages[idx].timestamp) == time_to_index.end()) {
                time_to_index[messages[idx].timestamp] = time_to_index.size();
            }
        }

        std::string time_dim = "time_g" + std::to_string(gi);
        const size_t level_count = level_to_index.size();
        const size_t time_count = time_to_index.size();
        const bool complete_level_time_grid = (level_count * time_count == g.frame_count);

        meta << "{\"id\":" << gi
             << ",\"name\":\"" << json_escape(g.label) << "\""
             << ",\"short_name\":\"" << json_escape(g.short_name) << "\""
             << ",\"type_of_level\":\"" << json_escape(g.type_of_level) << "\""
             << ",\"level\":\"" << json_escape(g.level) << "\""
             << ",\"stream_index\":" << gi
             << ",\"frame_start\":0"
             << ",\"frame_count\":" << g.frame_count
             << ",\"level_count\":" << level_count
             << ",\"time_count\":" << time_count
             << ",\"value_min\":" << g.value_min
             << ",\"value_max\":" << g.value_max
             << ",\"layout\":{\"axes\":[\"time\",\"level\"],\"shape\":["
             << time_count << "," << level_count
             << "],\"complete\":" << (complete_level_time_grid ? "true" : "false")
             << ",\"order\":\"time-major\"}"
             << ",\"options\":{\"bits\":" << opts.bits
             << ",\"min_value\":" << g.value_min
             << ",\"max_value\":" << g.value_max
             << ",\"storage_shift\":" << storage_shift
             << ",\"encoded_width\":" << ri.encoded_nx
             << ",\"encoded_height\":" << ri.encoded_ny
             << ",\"crop_width\":" << ri.crop_nx
             << ",\"crop_height\":" << ri.crop_ny
             << ",\"quantization\":\"" << group_quant_mode << "\""
             << ",\"chroma\":\"" << json_escape(group_chroma) << "\""
             << ",\"apply_log2\":" << (ri.apply_log2 ? "true" : "false")
             << ",\"apply_asinh\":" << (ri.apply_asinh ? "true" : "false")
             << ",\"log_offset\":" << ri.log_offset
             << ",\"apply_shifted_log\":" << (ri.apply_shifted_log?"true":"false")
             << ",\"transform_scale\":" << ri.transform_scale
             << ",\"transform_origin\":" << ri.transform_origin
             << ",\"transform_span\":" << ri.transform_span
             << ",\"allow_nan\":true"
             << ",\"frame_rate\":1"
             << ",\"flip_ud\":false"
             << ",\"codec\":\"" << json_escape(opts.codec) << "\""
             << ",\"quality\":\"" << json_escape(opts.quality) << "\"";
        if (!opts.encoder.empty()) {
            meta << ",\"encoder\":\"" << json_escape(opts.encoder) << "\"";
        }
        if (ri.shared_bitmap_present) {
            meta << ",\"shared_bitmap\":{\"encoding\":\"zlib+base64\",\"dtype\":\"uint8\",\"shape\":["
                 << (static_cast<size_t>(g.nx) * static_cast<size_t>(g.ny))
                 << "],\"data\":\"" << ri.shared_bitmap_payload << "\"}";
        }
        meta << "}"
             << ",\"dims\":[\"" << time_dim << "\",\"lat\",\"lon\"]"
             << ",\"coords\":{\"" << time_dim << "\":{\"data\":" << encode_time_payload(times)
             << ",\"attrs\":{\"standard_name\":\"time\"}}}";

        meta << ",\"records\":[";
        for (size_t r = 0; r < g.message_indices_sorted.size(); ++r) {
            if (r) meta << ',';
            const auto &m = messages[g.message_indices_sorted[r]];
            auto quant_range = quant_range_for_assignment(opts, g, m, messages, &ri, r);
            const double record_min = quant_range.first;
            const double record_max = quant_range.second;
            const int shift_bits = (r < ri.frame_shift_bits.size()) ? ri.frame_shift_bits[r] : 0;
            meta << "{\"file_index\":" << m.file_index
                 << ",\"message_index\":" << m.message_index
                 << ",\"frame_index\":" << r
                 << ",\"time_index\":" << time_to_index[m.timestamp]
                 << ",\"level_index\":" << level_to_index[m.level]
                 << ",\"short_name\":\"" << json_escape(m.short_name) << "\""
                 << ",\"typeOfLevel\":\"" << json_escape(m.type_of_level) << "\""
                 << ",\"level\":\"" << json_escape(m.level) << "\""
                 << ",\"number\":\"" << json_escape(m.ensemble_number) << "\""
                 << ",\"perturbationNumber\":\"" << json_escape(m.perturbation_number) << "\""
                 << ",\"timestamp\":\"" << json_escape(m.timestamp) << "\""
                 << ",\"dataDate\":\"" << json_escape(m.data_date) << "\""
                 << ",\"dataTime\":\"" << json_escape(m.data_time) << "\""
                 << ",\"stepRange\":\"" << json_escape(m.step_range) << "\""
                 << ",\"value_min\":" << m.value_min
                 << ",\"value_max\":" << m.value_max
                 << ",\"quantization_min\":" << record_min
                 << ",\"quantization_max\":" << record_max
                 << ",\"bitmapPresent\":" << (m.bitmap_present ? "true" : "false")
                 << ",\"binary_scale_shift\":" << shift_bits
                 << "}"
                 ;
        }
        meta << "]";

        meta << "}";
    }
    meta << "]";

    meta << "}";
    return meta.str();
}

std::string build_comment_binary_v2(const Options &opts,
                                    const std::vector<MessageMeta> &messages,
                                    const std::vector<Group> &groups,
                                    const std::vector<GroupRuntimeInfo> &runtime_infos) {
    if (groups.empty()) throw std::runtime_error("No groups for AVENCv2 metadata");
    if (runtime_infos.size() != groups.size()) {
        throw std::runtime_error("runtime metadata size mismatch for AVENCv2");
    }
    const int nx = groups.front().nx;
    const int ny = groups.front().ny;
    const int storage_shift = storage_shift_for_bits(opts);
    const auto latitudes = build_latitudes(groups.front());
    const auto longitudes = build_longitudes(groups.front());

    std::vector<Assignment> msg_to_assignment(messages.size(), Assignment{-1, 0});
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        for (size_t fi = 0; fi < g.message_indices_sorted.size(); ++fi) {
            const size_t mi = g.message_indices_sorted[fi];
            msg_to_assignment[mi] = Assignment{static_cast<int>(gi), fi};
        }
    }

    std::vector<std::string> stream_templates(groups.size(), "");
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        if (!g.message_indices_sorted.empty()) {
            const auto &m0 = messages[g.message_indices_sorted.front()];
            stream_templates[gi] = m0.template_b64;
        }
    }

    size_t record_count = 0;
    for (const auto &asg : msg_to_assignment) {
        if (asg.group_id >= 0) ++record_count;
    }

    std::vector<uint8_t> payload;
    payload.reserve(64u + groups.size() * 256u + record_count * 320u);

    // AVENCv2 binary layout (little-endian):
    // magic[8]="AV2META\0", format_version(u32)=2/3/4, flags(u32),
    // nx(u32), ny(u32), stream_count(u32), record_count(u32), reserved(u32),
    // stream table, then record table.
    const char magic[8] = {'A', 'V', '2', 'M', 'E', 'T', 'A', '\0'};
    payload.insert(payload.end(), magic, magic + 8);
    bool extended_transform=false,any_asinh=false;
    for(const auto& g:groups){extended_transform|=g.apply_shifted_log||(g.apply_asinh&&g.transform_scale!=1);any_asinh|=g.apply_asinh;}
    append_u32le(payload, extended_transform?4u:any_asinh?3u:2u); // rev3: unit asinh; rev4: scaled/shifted transforms and 3 Float64 parameters
    append_u32le(payload, 1u); // flags bit 0: appended latitude/longitude arrays
    append_u32le(payload, static_cast<uint32_t>(nx));
    append_u32le(payload, static_cast<uint32_t>(ny));
    append_u32le(payload, to_u32_size(groups.size(), "stream_count"));
    append_u32le(payload, to_u32_size(record_count, "record_count"));
    append_u32le(payload, 0u); // reserved

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        const auto &ri = runtime_infos[gi];
        const std::string bitmap_payload = ri.shared_bitmap_present ? ri.shared_bitmap_payload : "";

        append_i32le(payload, static_cast<int32_t>(gi));
        append_u32le(payload, to_u32_size(g.frame_count, "stream.frame_count"));
        append_u32le(payload, static_cast<uint32_t>(opts.bits));
        append_i32le(payload, storage_shift);
        append_f64le(payload, g.value_min);
        append_f64le(payload, g.value_max);
        append_u32le(payload, avenc_transform::mode(ri.apply_log2,ri.apply_asinh,ri.apply_shifted_log,ri.transform_scale));
        append_f64le(payload, ri.log_offset);
        if(extended_transform){append_f64le(payload,ri.transform_scale);append_f64le(payload,ri.transform_origin);append_f64le(payload,ri.transform_span);}
        append_u32le(payload, static_cast<uint32_t>(ri.encoded_nx));
        append_u32le(payload, static_cast<uint32_t>(ri.encoded_ny));
        append_u32le(payload, static_cast<uint32_t>(ri.crop_nx));
        append_u32le(payload, static_cast<uint32_t>(ri.crop_ny));
        append_string_u32(payload, bitmap_payload);
        append_string_u32(payload, stream_templates[gi]);
    }

    for (size_t mi = 0; mi < messages.size(); ++mi) {
        const auto &m = messages[mi];
        const auto &asg = msg_to_assignment[mi];
        if (asg.group_id < 0) continue;
        const size_t gi = static_cast<size_t>(asg.group_id);
        const auto &g = groups[gi];
        const auto& opts=g.encoding_options;
        const int storage_shift=storage_shift_for_bits(opts);
        const auto &ri = runtime_infos[gi];
        const auto quant_range = quant_range_for_assignment(
            opts, g, m, messages, &ri, asg.frame_index_in_group);
        const double record_min = quant_range.first;
        const double record_max = quant_range.second;
        const int shift_bits = (asg.frame_index_in_group < ri.frame_shift_bits.size())
                                   ? ri.frame_shift_bits[asg.frame_index_in_group]
                                   : 0;
        std::string record_template;
        if (!m.template_b64.empty() && m.template_b64 != stream_templates[gi]) {
            record_template = m.template_b64;
        }

        append_u64le(payload, static_cast<uint64_t>(m.global_order));
        append_i32le(payload, asg.group_id);
        append_u32le(payload, to_u32_size(asg.frame_index_in_group, "record.frame_index"));
        append_i32le(payload, m.file_index);
        append_u64le(payload, static_cast<uint64_t>(m.message_index));
        append_f64le(payload, record_min);
        append_f64le(payload, record_max);
        append_u32le(payload, m.bitmap_present ? 1u : 0u);
        append_i32le(payload, m.bits_per_value);
        append_i32le(payload, shift_bits);
        append_string_u32(payload, m.short_name);
        append_string_u32(payload, m.type_of_level);
        append_string_u32(payload, m.level);
        append_string_u32(payload, m.data_date);
        append_string_u32(payload, m.data_time);
        append_string_u32(payload, m.step_range);
        append_string_u32(payload, m.timestamp);
        append_string_u32(payload, m.ensemble_number);
        append_string_u32(payload, m.perturbation_number);
        append_string_u32(payload, record_template);
    }

    // Optional extension section. Readers of the original v2 layout stop after the record
    // table and safely ignore these bytes; coordinate-aware readers use flags bit 0.
    append_u32le(payload, to_u32_size(latitudes.size(), "latitude_count"));
    for (float value : latitudes) append_f64le(payload, static_cast<double>(value));
    append_u32le(payload, to_u32_size(longitudes.size(), "longitude_count"));
    for (float value : longitudes) append_f64le(payload, static_cast<double>(value));

    return "AVENCv2|" + zlib_base64_payload(payload);
}

std::vector<std::string> build_group_encode_args(const Options &opts,
                                                 const std::string &raw_input,
                                                 const std::string &output_path,
                                                 int nx,
                                                 int ny) {
    std::vector<std::string> args;
    args.emplace_back("ffmpeg");
    args.emplace_back("-y");
    args.emplace_back("-loglevel");
    args.emplace_back("error");
    args.emplace_back("-f");
    args.emplace_back("rawvideo");
    args.emplace_back("-pix_fmt");
    args.emplace_back(avenc_raw_gray_format(opts.bits, storage_shift_for_bits(opts)));
    args.emplace_back("-video_size");
    args.emplace_back(std::to_string(nx) + "x" + std::to_string(ny));
    args.emplace_back("-framerate");
    args.emplace_back("1");
    args.emplace_back("-i");
    args.emplace_back(raw_input);

    for (const auto &kv : encoder_options(opts, nx, ny)) {
        args.emplace_back(kv.first);
        args.emplace_back(kv.second);
    }
    for (const auto &tok : opts.ffmpeg_extra_args) {
        args.emplace_back(tok);
    }

    std::string enc_name = ffmpeg_codec_name(opts);
    args.emplace_back("-c:v");
    args.emplace_back(enc_name);
    if (enc_name == "libx265") {
        args.emplace_back("-x265-params");
        args.emplace_back("log-level=error");
    }

    args.emplace_back("-movflags");
    args.emplace_back("+faststart");
    args.emplace_back("-r");
    args.emplace_back("1");
    args.emplace_back("-f");
    args.emplace_back("mp4");
    args.emplace_back(output_path);

    return args;
}

std::vector<std::string> build_mux_streams_args(const std::vector<std::string> &group_videos,
                                                const std::string &output_path) {
    std::vector<std::string> args;
    args.emplace_back("ffmpeg");
    args.emplace_back("-y");
    args.emplace_back("-loglevel");
    args.emplace_back("error");
    for (const auto &video : group_videos) {
        args.emplace_back("-i");
        args.emplace_back(video);
    }
    for (size_t i = 0; i < group_videos.size(); ++i) {
        args.emplace_back("-map");
        args.emplace_back(std::to_string(i) + ":v:0");
    }
    args.emplace_back("-c");
    args.emplace_back("copy");
    args.emplace_back("-map_metadata");
    args.emplace_back("-1");
    args.emplace_back("-f");
    args.emplace_back("mp4");
    args.emplace_back(output_path);

    return args;
}

std::vector<std::string> build_attach_metadata_args(const std::string &input_mux_path,
                                                    const std::string &metadata_path,
                                                    const std::string &output_path) {
    std::vector<std::string> args;
    args.emplace_back("ffmpeg");
    args.emplace_back("-y");
    args.emplace_back("-loglevel");
    args.emplace_back("error");
    args.emplace_back("-i");
    args.emplace_back(input_mux_path);
    args.emplace_back("-f");
    args.emplace_back("ffmetadata");
    args.emplace_back("-i");
    args.emplace_back(metadata_path);
    args.emplace_back("-map");
    args.emplace_back("0");
    args.emplace_back("-c");
    args.emplace_back("copy");
    args.emplace_back("-map_metadata");
    args.emplace_back("1");
    args.emplace_back("-movflags");
    args.emplace_back("+faststart");
    args.emplace_back("-f");
    args.emplace_back("mp4");
    args.emplace_back(output_path);

    return args;
}

void print_verbose_report(const RunStats &stats) {
    std::cout << "\nCompression report\n";
    std::cout << "  streams: " << stats.stream_count << "\n";
    std::cout << "  messages: " << stats.message_count << "\n";
    std::cout << "  compressor: " << stats.compressor << "\n";
    std::cout << "  scan_mode: " << (stats.headers_only_scan ? "headers_only(deferred_templates)" : "eager_templates")
              << " (scan_template_sec=" << std::fixed << std::setprecision(3) << stats.template_scan_sec
              << " s, deferred_template_sec=" << stats.template_deferred_sec << " s)\n";
    std::cout << "  fast_path: " << (stats.fast_path_enabled ? "enabled" : "disabled")
              << " (" << stats.fast_path_frames << "/" << stats.encoded_frames << " frames)\n";
    std::cout << "  sorting_duration: " << std::fixed << std::setprecision(3)
              << stats.sorting_sec << " s"
              << " (scan=" << stats.scan_sec << " s, group=" << stats.group_build_sec << " s)\n";
    std::cout << "  compression_duration: " << std::fixed << std::setprecision(3)
              << stats.compression_sec << " s\n";
    std::cout << "  input_size: " << format_bytes(stats.input_bytes)
              << " (" << stats.input_bytes << " B)\n";
    std::cout << "  output_size: " << format_bytes(stats.output_bytes)
              << " (" << stats.output_bytes << " B)\n";
    std::cout << "  compression_ratio: " << std::fixed << std::setprecision(3)
              << stats.compression_ratio << "\n";
    std::cout << "  metadata_in_mp4_size: " << format_bytes(stats.metadata_comment_bytes)
              << " (" << stats.metadata_comment_bytes << " B)\n";
    std::cout << "  metadata_json_size: " << format_bytes(stats.metadata_json_bytes)
              << " (" << stats.metadata_json_bytes << " B)\n";
    std::cout << "  ffmetadata_file_size: " << format_bytes(stats.metadata_file_bytes)
              << " (" << stats.metadata_file_bytes << " B)\n";
    std::cout << "  temp_metadata_size: " << format_bytes(stats.metadata_file_bytes)
              << " (" << stats.metadata_file_bytes << " B)\n";
    std::cout << "  temp_raw_size: " << format_bytes(stats.temp_raw_bytes)
              << " (" << stats.temp_raw_bytes << " B)\n";
    std::cout << "  temp_group_video_size: " << format_bytes(stats.temp_group_video_bytes)
              << " (" << stats.temp_group_video_bytes << " B)\n";
    std::cout << "  temp_mux_size: " << format_bytes(stats.temp_mux_bytes)
              << " (" << stats.temp_mux_bytes << " B)\n";
    std::cout << "  temp_total_size: " << format_bytes(stats.temp_total_bytes)
              << " (" << stats.temp_total_bytes << " B)\n";
    std::cout << "  peak_memory: " << std::fixed << std::setprecision(2)
              << stats.peak_rss_mb << " MiB\n";
}

} // namespace

int main(int argc, char **argv) {
    if (avenc_print_version(argc, argv)) return 0;
    try {
        using clock = std::chrono::steady_clock;
        Options opts;
        parse_args(argc, argv, opts);
        if (opts.mask_fill != "min" && opts.mask_fill != "taper3")
            throw std::runtime_error("--mask-fill must be min or taper3");
        RunStats stats;
        stats.headers_only_scan = opts.headers_only_scan;
        stats.fast_path_enabled = opts.fast_path;
        stats.input_bytes = 0;
        for (const auto &in : opts.inputs) stats.input_bytes += file_size_bytes(in);

        std::vector<MessageMeta> messages;
        ScanProfile scan_profile;
        double global_min = 0.0;
        double global_max = 0.0;
        const uint64_t metadata_units_total = std::max<uint64_t>(1, stats.input_bytes + 1);
        ProgressBar metadata_progress("step 1/2 metadata", metadata_units_total, !opts.quiet);
        const auto t_scan0 = clock::now();
        scan_messages(
            opts,
            messages,
            global_min,
            global_max,
            stats.input_bytes,
            &scan_profile,
            [&](uint64_t done, uint64_t total) {
                const uint64_t cap_total = std::max<uint64_t>(1, total);
                metadata_progress.update(std::min(done, cap_total));
            });
        const auto t_scan1 = clock::now();

        if (messages.empty()) {
            metadata_progress.finish();
            std::cerr << "No GRIB messages found in input files\n";
            return 1;
        }

        std::string chosen_mode = detect_group_mode(messages, opts.group_by);
        opts.group_by = chosen_mode;

        const auto t_group0 = clock::now();
        std::vector<Group> groups = build_groups(messages, chosen_mode);
        prepare_shared_bitmaps(opts,messages,groups);
        const auto t_group1 = clock::now();

        if (groups.empty()) {
            metadata_progress.finish();
            std::cerr << "No groups were built from input messages\n";
            return 1;
        }

        metadata_progress.update(metadata_units_total);
        metadata_progress.finish();

        stats.message_count = messages.size();
        stats.encoded_frames = messages.size();
        stats.stream_count = groups.size();
        stats.template_scan_sec = scan_profile.template_sec;
        stats.scan_sec = std::chrono::duration<double>(t_scan1 - t_scan0).count();
        stats.group_build_sec = std::chrono::duration<double>(t_group1 - t_group0).count();
        stats.sorting_sec = std::chrono::duration<double>(t_group1 - t_scan0).count();
        std::ostringstream compressor;
        compressor << ffmpeg_codec_name(opts)
                   << " (codec=" << opts.codec
                   << ", quality=" << opts.quality
                   << ", bits=" << opts.bits
                   << ", chroma=" << opts.chroma;
        if (!opts.encoder.empty()) compressor << ", encoder=" << opts.encoder;
        compressor << ")";
        stats.compressor = opts.recipes_path.empty()?compressor.str():"per-stream recipes (unmatched streams use "+compressor.str()+")";

        if (!opts.quiet) {
            if (opts.verbose) {
                std::cout << "Scanned " << messages.size() << " messages across "
                          << opts.inputs.size() << " files\n";
                std::cout << "Grouping strategy: " << chosen_mode
                          << " -> " << groups.size() << " groups\n";
                for (size_t i = 0; i < groups.size(); ++i) {
                    const auto &g = groups[i];
                    std::cout << "  [group " << i << "] " << g.label
                              << " shortName=" << g.short_name
                              << " typeOfLevel=" << g.type_of_level
                              << " level=" << g.level
                              << " frames=" << g.frame_count
                              << " frame_start=" << g.frame_start
                              << " codec=" << g.encoding_options.codec
                              << " bits=" << g.encoding_options.bits
                              << " transform=" << g.encoding_options.transform
                              << " quantization=" << g.encoding_options.quantization
                              << " range=[" << g.value_min << ", " << g.value_max << "]"
                              << "\n";
                }
                std::cout << "[profile] scan_phase_sec="
                          << std::chrono::duration<double>(t_scan1 - t_scan0).count()
                          << " build_groups_sec="
                          << std::chrono::duration<double>(t_group1 - t_group0).count()
                          << "\n";
            } else {
                std::cout << messages.size()
                          << " messages in " << opts.inputs.size()
                          << " files to be compressed in " << groups.size()
                          << " groups (" << chosen_mode << " strategy)\n";
            }
            std::cout << std::flush;
        }

        const auto t_comp0 = clock::now();
        std::vector<GroupRuntimeInfo> runtime_infos;
        std::vector<std::string> group_video_paths(groups.size());
        for (size_t gid = 0; gid < groups.size(); ++gid) {
            char video_template[] = "/tmp/avpack_group_videoXXXXXX.mp4";
            int fd = mkstemps(video_template, 4);
            if (fd == -1) throw std::runtime_error("Unable to create temporary group video path");
            close(fd);
            std::remove(video_template);
            group_video_paths[gid] = video_template;

            const std::string effective_chroma =
                effective_chroma_for_group(opts, groups[gid].nx, groups[gid].ny);
            if (!opts.quiet && effective_chroma != opts.chroma) {
                std::cout << "[encode] group=" << groups[gid].label
                          << " requested_chroma=" << opts.chroma
                          << " -> effective_chroma=" << effective_chroma
                          << " (odd grid dimensions)\n";
            }
        }

        const uint64_t compression_units_total =
            static_cast<uint64_t>(messages.size()) +
            static_cast<uint64_t>(groups.size()) + 2u;
        ProgressBar compression_progress("step 2/2 compression", compression_units_total, !opts.quiet);
        uint64_t compression_units_done = 0;
        auto on_frame_done = [&](uint64_t delta) {
            compression_units_done += delta;
            compression_progress.update(compression_units_done);
        };

        runtime_infos.assign(groups.size(), GroupRuntimeInfo{});
        uint64_t fastpath_frames_total = 0;
        std::unordered_map<std::string, std::string> deferred_template_cache;
        for (size_t gid = 0; gid < groups.size(); ++gid) {
            fastpath_frames_total += static_cast<uint64_t>(encode_single_group_streaming(
                groups[gid].encoding_options,
                messages,
                groups[gid],
                group_video_paths[gid],
                runtime_infos[gid],
                &deferred_template_cache,
                &stats.template_deferred_sec,
                on_frame_done));
            ++compression_units_done;
            compression_progress.update(compression_units_done);
        }
        stats.fast_path_frames = fastpath_frames_total;

        std::string comment;
        if (opts.metadata_version == 2) {
            stats.metadata_json_bytes = 0;
            comment = build_comment_binary_v2(opts, messages, groups, runtime_infos);
        } else {
            std::string metadata_json =
                build_metadata_json(opts, opts.inputs, messages, groups, runtime_infos);
            stats.metadata_json_bytes = metadata_json.size();
            comment = compress_comment_json_v1(metadata_json);
        }
        stats.metadata_comment_bytes = comment.size();
        std::string metadata_path = write_ffmetadata_file(comment);
        if (metadata_path.empty()) {
            compression_progress.finish();
            throw std::runtime_error("Failed to create ffmetadata file");
        }
        stats.metadata_file_bytes = file_size_bytes(metadata_path);

        char mux_template[] = "/tmp/avpack_muxXXXXXX.mp4";
        int mux_fd = mkstemps(mux_template, 4);
        if (mux_fd == -1) throw std::runtime_error("Unable to create temporary mux path");
        close(mux_fd);
        std::remove(mux_template);
        std::string mux_path = mux_template;

        auto mux_args = build_mux_streams_args(group_video_paths, mux_path);
        int mux_ret = run_command(mux_args, opts.verbose);
        if (mux_ret != 0) {
            compression_progress.finish();
            std::remove(metadata_path.c_str());
            for (const auto &p : group_video_paths) std::remove(p.c_str());
            std::cerr << "ffmpeg stream mux exited with code " << mux_ret << "\n";
            return 1;
        }
        ++compression_units_done;
        compression_progress.update(compression_units_done);

        auto final_args = build_attach_metadata_args(mux_path, metadata_path, opts.output_path);
        int final_ret = run_command(final_args, opts.verbose);
        ++compression_units_done;
        compression_progress.update(compression_units_done);
        compression_progress.finish();

        stats.temp_group_video_bytes = 0;
        for (const auto &p : group_video_paths) stats.temp_group_video_bytes += file_size_bytes(p);
        stats.temp_mux_bytes = file_size_bytes(mux_path);
        stats.temp_total_bytes =
            stats.temp_raw_bytes +
            stats.temp_group_video_bytes +
            stats.temp_mux_bytes +
            stats.metadata_file_bytes;

        std::remove(mux_path.c_str());
        std::remove(metadata_path.c_str());
        for (const auto &p : group_video_paths) std::remove(p.c_str());

        if (final_ret != 0) {
            std::cerr << "ffmpeg metadata attach exited with code " << final_ret << "\n";
            return 1;
        }

        const auto t_comp1 = clock::now();
        stats.compression_sec = std::chrono::duration<double>(t_comp1 - t_comp0).count();
        stats.output_bytes = file_size_bytes(opts.output_path);
        stats.compression_ratio =
            (stats.output_bytes > 0) ? (static_cast<double>(stats.input_bytes) /
                                        static_cast<double>(stats.output_bytes))
                                     : 0.0;
        stats.peak_rss_mb = peak_rss_mb();

        if (!opts.quiet) {
            std::cout << "Packed AVENC video written to " << opts.output_path << "\n";
        }
        if (opts.verbose) {
            print_verbose_report(stats);
        }

        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "avpack error: " << ex.what() << "\n";
        return 1;
    }
}
