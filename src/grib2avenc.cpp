#include "avenc_version.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

struct EncodeProfile {
    std::string name;
    std::string codec;
    std::string quality;
    int bits = 8;
    std::string encoder;
    std::string note;
    std::string quantization = "auto";
    std::string chroma = "420";
    std::vector<std::string> ffmpeg_opts;
};

const std::vector<EncodeProfile> kProfiles = {
    {"avc_xcr8", "avc", "verylow", 8, "", "AVC high compression ratio"},
    {"avc_fast10", "avc", "fast", 10, "", "AVC fast compute 10-bit"},
    {"avc_good8", "avc", "average", 8, "", "AVC good quality"},
    {"avc_vgood10", "avc", "high", 10, "", "AVC very good quality"},
    {"avc_vgood12", "avc", "veryhigh", 12, "", "AVC very good quality 12-bit"},

    {"hevc_xcr8", "hevc", "verylow", 8, "", "HEVC high compression ratio"},
    {"hevc_fast10", "hevc", "fast", 10, "", "HEVC fast compute 10-bit"},
    {"hevc_good10", "hevc", "average", 10, "", "HEVC good quality"},
    {"hevc_hq10", "hevc", "high", 10, "", "HEVC high quality"},
    {"hevc_vgood12", "hevc", "veryhigh", 12, "", "HEVC very good quality"},

    {"av1_xcr10", "av1", "verylow", 10, "libsvtav1", "AV1 high compression ratio"},
    {"av1_fast", "av1", "fast", 10, "libsvtav1", "AV1 fast compute 10-bit"},
    {"av1_good10", "av1", "average", 10, "libsvtav1", "AV1 good quality"},
    {"av1_hq10", "av1", "high", 10, "libsvtav1", "AV1 high quality"},
    {"av1_vgood12", "av1", "veryhigh", 12, "libsvtav1", "AV1 very good quality"},
    {"av1_lossless12_frameMinMax_400", "av1", "lossless", 12, "libaom-av1",
     "Lossless AV1 video; 12-bit scientific range quantization still applies",
     "frameMinMax", "400",
     {"-cpu-used", "4", "-row-mt", "1", "-g", "16", "-lag-in-frames", "0", "-aq-mode", "0"}},
    {"av1_lossless12_auto_400", "av1", "lossless", 12, "libaom-av1",
     "Lossless AV1 video; 12-bit scientific range quantization still applies",
     "auto", "400",
     {"-cpu-used", "4", "-row-mt", "1", "-g", "16", "-lag-in-frames", "0", "-aq-mode", "0"}},
    {"av1_good10_auto_400", "av1", "average", 10, "libsvtav1",
     "AV1 good10 automatic range yuv400", "auto", "400"},
    {"vp9_hq10_auto_420", "vp9", "high", 10, "libvpx-vp9",
     "VP9 hq10 automatic range yuv420", "auto", "420"},

    {"vp9_fast8", "vp9", "fast", 8, "libvpx-vp9", "VP9 fast compute"},
    {"vp9_fast10", "vp9", "fast", 10, "libvpx-vp9", "VP9 fast compute 10-bit"},
    {"vp9_xcr10", "vp9", "verylow", 10, "libvpx-vp9", "VP9 very high compression ratio"},
    {"vp9_bal8", "vp9", "average", 8, "libvpx-vp9", "VP9 balanced 8-bit"},
    {"vp9_bal10", "vp9", "average", 10, "libvpx-vp9", "VP9 balanced 10-bit"},
    {"vp9_good10", "vp9", "average", 10, "libvpx-vp9", "VP9 good quality 10-bit"},
    {"vp9_bal12", "vp9", "average", 12, "libvpx-vp9", "VP9 balanced 12-bit"},
    {"vp9_hq10", "vp9", "high", 10, "libvpx-vp9", "VP9 high quality 10-bit"},
    {"vp9_hq12", "vp9", "high", 12, "libvpx-vp9", "VP9 high quality 12-bit"},
    {"vp9_vgood12", "vp9", "high", 12, "libvpx-vp9", "VP9 very good quality 12-bit"},
    {"vp9_uhq12", "vp9", "veryhigh", 12, "libvpx-vp9", "VP9 ultra high quality 12-bit"},
    {"vp9_lossless12", "vp9", "lossless", 12, "libvpx-vp9", "VP9 lossless 12-bit"},

    {"av1_good10_frameMinMax_400", "av1", "average", 10, "libsvtav1",
     "AV1 good10 frameMinMax yuv400", "frameMinMax", "400"},
    {"vp9_hq10_frameMinMax_400", "vp9", "high", 10, "libvpx-vp9",
     "VP9 hq10 frameMinMax yuv400", "frameMinMax", "400"},
    {"av1_good10_groupMinMax_400", "av1", "average", 10, "libsvtav1",
     "AV1 good10 groupMinMax yuv400", "groupMinMax", "400"},
    {"av1_fast_groupMinMax_400", "av1", "fast", 10, "libsvtav1",
     "AV1 fast groupMinMax yuv400", "groupMinMax", "400"},
    {"vp9_hq10_groupMinMax_400", "vp9", "high", 10, "libvpx-vp9",
     "VP9 hq10 groupMinMax yuv400", "groupMinMax", "400"},
    {"av1_good10_frameMinMax_420", "av1", "average", 10, "libsvtav1",
     "AV1 good10 frameMinMax yuv420", "frameMinMax", "420"},
    {"vp9_hq10_frameMinMax_420", "vp9", "high", 10, "libvpx-vp9",
     "VP9 hq10 frameMinMax yuv420", "frameMinMax", "420"},
    {"av1_good10_groupMinMax_420", "av1", "average", 10, "libsvtav1",
     "AV1 good10 groupMinMax yuv420", "groupMinMax", "420"},
    {"vp9_hq10_groupMinMax_420", "vp9", "high", 10, "libvpx-vp9",
     "VP9 hq10 groupMinMax yuv420", "groupMinMax", "420"},
};

[[noreturn]] void usage(const char *prog) {
    std::cerr << "Usage: " << prog
              << " [--enc <profile>] [--enc_config_table <profiles.csv>] [--threads <N>|-threads <N>] [--metadata-version <1|2>] [--fast-path|--no_fast_path] [--headers_only_scan|--no_headers_only_scan] [-v] <grib1> [<grib2> ...] [output.mp4]\n"
              << "Try '" << prog << " --help' for full help.\n";
    std::exit(1);
}

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::vector<std::string> parse_csv_line(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cur.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(trim(cur));
    return out;
}

bool is_quantization_mode(const std::string &s) {
    return s == "auto" || s == "frameMinMax" || s == "groupMinMax";
}

bool is_chroma_mode(const std::string &s) { return s == "400" || s == "420"; }

std::vector<EncodeProfile> load_profiles_from_table(const std::string &csv_path) {
    std::ifstream in(csv_path);
    if (!in) {
        throw std::runtime_error("Unable to open enc config table: " + csv_path);
    }
    std::map<std::string, EncodeProfile> by_name;
    for (const auto &p : kProfiles) by_name[p.name] = p;

    std::string line;
    size_t lno = 0;
    while (std::getline(in, line)) {
        ++lno;
        const std::string raw = trim(line);
        if (raw.empty() || raw[0] == '#') continue;
        auto cols = parse_csv_line(raw);
        if (cols.size() < 5) {
            throw std::runtime_error("Invalid enc config row at line " + std::to_string(lno) +
                                     ": expected at least 5 columns");
        }
        if (lno == 1) {
            const std::string c0 = cols[0];
            if (c0 == "name" || c0 == "profile" || c0 == "nickname") continue;
        }

        EncodeProfile p;
        p.name = trim(cols[0]);
        p.note = (cols.size() > 1) ? trim(cols[1]) : "";
        p.codec = (cols.size() > 2) ? trim(cols[2]) : "";
        p.quality = (cols.size() > 3) ? trim(cols[3]) : "";
        if (p.name.empty() || p.codec.empty() || p.quality.empty()) {
            throw std::runtime_error("Invalid enc config row at line " + std::to_string(lno) +
                                     ": name/codec/quality must be non-empty");
        }
        try {
            p.bits = std::stoi(trim(cols[4]));
        } catch (...) {
            throw std::runtime_error("Invalid bits value in enc config row at line " +
                                     std::to_string(lno) + ": '" + cols[4] + "'");
        }
        if (cols.size() > 5) p.encoder = trim(cols[5]);

        for (size_t i = 6; i < cols.size(); ++i) {
            std::string tok = trim(cols[i]);
            if (tok.empty()) continue;
            if (is_quantization_mode(tok)) {
                p.quantization = tok;
                continue;
            }
            if (is_chroma_mode(tok)) {
                p.chroma = tok;
                continue;
            }
            p.ffmpeg_opts.push_back(tok);
        }
        by_name[p.name] = p;
    }

    std::vector<EncodeProfile> out;
    out.reserve(by_name.size());
    for (const auto &kv : by_name) out.push_back(kv.second);
    return out;
}

const EncodeProfile &find_profile(const std::vector<EncodeProfile> &profiles, const std::string &name) {
    for (const auto &p : profiles) {
        if (p.name == name) return p;
    }
    throw std::runtime_error("Unknown encoding profile: " + name);
}

struct ProfileSelection {
    std::string base_name;
    int forced_crf = -1;
};

struct CrfBounds {
    bool known = false;
    int min = 0;
    int max = 0;
};

CrfBounds crf_bounds_for_profile(const EncodeProfile &p) {
    const std::string codec = trim(p.codec);
    const std::string encoder = trim(p.encoder);
    if (codec == "avc" || codec == "h264" || codec == "hevc" || codec == "h265" ||
        encoder == "libx264" || encoder == "libx265" || encoder == "libh265") {
        return {true, 0, 51};
    }
    if (codec == "av1" || codec == "vp9" ||
        encoder == "libaom-av1" || encoder == "libsvtav1" || encoder == "libvpx-vp9") {
        return {true, 0, 63};
    }
    return {};
}

ProfileSelection resolve_profile_selection(const std::vector<EncodeProfile> &profiles,
                                          const std::string &requested) {
    // Exact profile name has priority.
    for (const auto &p : profiles) {
        if (p.name == requested) return {requested, -1};
    }

    // Synthetic suffix syntax: <profile>_crf<NN>
    const std::regex re("^(.*)_crf([0-9]+)$");
    std::smatch m;
    if (!std::regex_match(requested, m, re)) {
        return {requested, -1};
    }
    const std::string base = m[1].str();
    const std::string crf_s = m[2].str();
    for (const auto &p : profiles) {
        if (p.name == base) {
            int crf = -1;
            try {
                crf = std::stoi(crf_s);
            } catch (...) {
                throw std::runtime_error("Invalid CRF suffix in profile name: " + requested);
            }
            return {base, crf};
        }
    }
    return {requested, -1};
}

void print_profiles(const std::vector<EncodeProfile> &profiles) {
    std::cout << "Available --enc profiles (" << profiles.size() << "):\n";
    for (const auto &p : profiles) {
        std::cout << "  " << p.name
                  << "  codec=" << p.codec
                  << " quality=" << p.quality
                  << " bits=" << p.bits
                  << " quant=" << p.quantization
                  << " chroma=" << p.chroma;
        if (!p.encoder.empty()) std::cout << " encoder=" << p.encoder;
        if (!p.ffmpeg_opts.empty()) std::cout << " ffopts=" << p.ffmpeg_opts.size();
        std::cout << "  # " << p.note << "\n";
    }
}

void print_help(const char *prog) {
    std::cout
        << "grib2avenc packs one or more GRIB files into a single MP4 container.\n"
        << "It leverages FFmpeg for video encoding/muxing and embeds AVENC metadata\n"
        << "to reconstruct GRIB messages later with avenc2grib.\n\n"
        << "Usage:\n"
        << "  " << prog << " [options] <grib1> [<grib2> ...] [output.mp4]\n\n"
        << "Behavior:\n"
        << "  - Default profile: av1_lossless12_auto_400 (same as --lossless)\n"
        << "  - If one input is provided and output is omitted: writes <input>.mp4\n"
        << "  - If multiple inputs are provided, output.mp4 is required\n\n"
        << "Options:\n"
        << "  --enc <profile>      Encoding profile to use\n"
        << "                       Supports suffix override: <profile>_crfNN (e.g. hevc_good10_crf42)\n"
        << "  --lossless           libaom AV1, gray12le, auto range, CRF 0, bitrate 0\n"
        << "                       CPU-used 4, row-MT, GOP 16, no lookahead\n"
        << "                       Lossless video only: scientific 12-bit quantization still applies\n"
        << "  --enc_config_table <profiles.csv>  Load/override profiles from CSV\n"
        << "  --mask-fill <min|taper3>  Missing-cell fill (default: taper3 for shared GRIB bitmaps)\n"
        << "  --threads <N>, -threads <N>  FFmpeg thread count override\n"
        << "  --headers_only_scan  Fast metadata scan (default): defer template extraction\n"
        << "  --no_headers_only_scan  Eager template extraction during scan\n"
        << "  --fast-path          Enable packed-integer fast path in avpack (default: disabled)\n"
        << "  --no_fast_path       Disable packed-integer fast path in avpack\n"
        << "  --metadata-version <1|2>  Metadata payload format (default: 2)\n"
        << "  --metadata-v1        Shortcut for --metadata-version 1\n"
        << "  --metadata-v2        Shortcut for --metadata-version 2\n"
        << "  --list-options       List available encoding profiles\n"
        << "  -v, --verbose        Verbose mode (shows progress and detailed stats report)\n"
        << "  -h, --help           Show this help\n\n"
        << "Quick shortcuts (all use automatic range selection):\n"
        << "  --lossless           av1_lossless12_auto_400 (quantization-only error)\n"
        << "  --high, -high        hevc_vgood12 (low additional codec error)\n"
        << "  --medium, -medium    vp9_hq10_auto_420 (balanced trade-off)\n"
        << "  --compact, -compact  av1_good10_auto_400 (compression priority)\n"
        << "  --fast, -fast        avc_xcr8 (speed priority)\n"
        << "  --low, -low          av1_good10_auto_400 (same as compact)\n"
        << "  -apple, --apple  hevc_xcr8\n\n"
        << "Examples:\n"
        << "  " << prog << " input.grib\n"
        << "  " << prog << " --enc vp9_hq10 input.grib output.mp4\n"
        << "  " << prog << " --enc hevc_good10_crf42 input.grib\n"
        << "  " << prog << " --enc_config_table custom_table.csv --enc avc_xcr10 input.grib\n"
        << "  " << prog << " -v input1.grib input2.grib output.mp4\n"
        << "  " << prog << " --list-options\n\n";
    print_profiles(kProfiles);
    std::cout << "\nAdvanced packer options are available via: avpack --help\n";
}

int run_command(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv.data());
        std::perror("execvp");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

bool encoder_supports_12bit_libx264() {
    // Probe ffmpeg's libx264 capabilities once; if probing fails, keep requested profile.
    FILE *pipe = popen("ffmpeg -hide_banner -h encoder=libx264 2>&1", "r");
    if (!pipe) return true;
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out.append(buf);
    }
    const int rc = pclose(pipe);
    if (rc != 0) return true;
    return out.find("yuv420p12le") != std::string::npos ||
           out.find("yuv422p12le") != std::string::npos ||
           out.find("yuv444p12le") != std::string::npos ||
           out.find("gray12le") != std::string::npos;
}

std::string dirname_of(const std::string &path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

} // namespace

int main(int argc, char **argv) {
    if (avenc_print_version(argc, argv)) return 0;
    if (argc < 2) usage(argv[0]);
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_help(argv[0]);
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--list-options") {
        print_profiles(kProfiles);
        return 0;
    }

    std::string profile_name = "av1_lossless12_auto_400";
    std::string enc_config_table;
    std::vector<std::string> positionals;
    bool verbose = false;
    bool fast_path = false;
    bool headers_only_scan = true;
    int metadata_version = 2;
    std::string mask_fill = "taper3";
    int threads = 0;
    bool list_options = false;

    std::map<std::string, std::string> legacy = {
        {"-fast", "avc_xcr8"},
        {"--fast", "avc_xcr8"},
        {"-medium", "vp9_hq10_auto_420"},
        {"--medium", "vp9_hq10_auto_420"},
        {"-high", "hevc_vgood12"},
        {"--high", "hevc_vgood12"},
        {"-low", "av1_good10_auto_400"},
        {"--low", "av1_good10_auto_400"},
        {"-compact", "av1_good10_auto_400"},
        {"--compact", "av1_good10_auto_400"},
        {"-apple", "hevc_xcr8"},
        {"--apple", "hevc_xcr8"},
        {"--lossless", "av1_lossless12_auto_400"},
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--enc" && i + 1 < argc) {
            profile_name = argv[++i];
        } else if (arg == "--mask-fill" && i + 1 < argc) {
            mask_fill = argv[++i];
        } else if (arg == "--enc_config_table" && i + 1 < argc) {
            enc_config_table = argv[++i];
        } else if ((arg == "--threads" || arg == "-threads") && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (arg == "--list-options") {
            list_options = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--headers_only_scan") {
            headers_only_scan = true;
        } else if (arg == "--no_headers_only_scan") {
            headers_only_scan = false;
        } else if (arg == "--fast-path" || arg == "--fast_path") {
            fast_path = true;
        } else if (arg == "--no_fast_path" || arg == "--no-fast-path") {
            fast_path = false;
        } else if (arg == "--metadata-version" && i + 1 < argc) {
            metadata_version = std::atoi(argv[++i]);
        } else if (arg == "--metadata-v1") {
            metadata_version = 1;
        } else if (arg == "--metadata-v2") {
            metadata_version = 2;
        } else if (legacy.find(arg) != legacy.end()) {
            profile_name = legacy[arg];
        } else if (!arg.empty() && arg[0] == '-') {
            usage(argv[0]);
        } else {
            positionals.push_back(arg);
        }
    }

    std::vector<EncodeProfile> profiles =
        enc_config_table.empty() ? kProfiles : load_profiles_from_table(enc_config_table);
    if (list_options) {
        print_profiles(profiles);
        return 0;
    }

    if (positionals.empty()) usage(argv[0]);

    std::vector<std::string> inputs;
    std::string output;
    if (positionals.size() == 1) {
        inputs = positionals;
        output = positionals.front() + ".mp4";
    } else {
        output = positionals.back();
        inputs.assign(positionals.begin(), positionals.end() - 1);
        if (output.size() < 4 || output.substr(output.size() - 4) != ".mp4") {
            throw std::runtime_error(
                "Output file must end with .mp4 when multiple input files are provided");
        }
    }
    if (inputs.empty()) usage(argv[0]);
    if (!(metadata_version == 1 || metadata_version == 2)) {
        throw std::runtime_error("metadata-version must be 1 or 2");
    }
    if (mask_fill != "min" && mask_fill != "taper3")
        throw std::runtime_error("--mask-fill must be min or taper3");
    if (threads < 0) {
        throw std::runtime_error("threads must be >= 0");
    }

    const ProfileSelection profile_sel = resolve_profile_selection(profiles, profile_name);
    const EncodeProfile &profile = find_profile(profiles, profile_sel.base_name);
    EncodeProfile effective_profile = profile;
    bool avc_12bit_fallback = false;
    const bool is_avc = (effective_profile.codec == "avc" || effective_profile.codec == "h264");
    const bool uses_libx264 = effective_profile.encoder.empty() || effective_profile.encoder == "libx264";
    if (is_avc && uses_libx264 && effective_profile.bits == 12 &&
        !encoder_supports_12bit_libx264()) {
        effective_profile.bits = 10;
        avc_12bit_fallback = true;
    }

    std::string exe_dir = dirname_of(argv[0] ? argv[0] : "");
    std::string avpack_local = exe_dir + "/avpack";
    const bool use_local = (access(avpack_local.c_str(), X_OK) == 0);

    std::vector<std::string> cmd;
    cmd.push_back(use_local ? avpack_local : "avpack");
    cmd.push_back("--out");
    cmd.push_back(output);
    cmd.push_back("--mask-fill");
    cmd.push_back(mask_fill);
    cmd.push_back("--group-by");
    cmd.push_back("var");
    cmd.push_back("--codec");
    cmd.push_back(effective_profile.codec);
    cmd.push_back("--quality");
    cmd.push_back(effective_profile.quality);
    cmd.push_back("--bits");
    cmd.push_back(std::to_string(effective_profile.bits));
    cmd.push_back("--quantization");
    cmd.push_back(effective_profile.quantization);
    cmd.push_back("--chroma");
    cmd.push_back(effective_profile.chroma);
    cmd.push_back("--metadata-version");
    cmd.push_back(std::to_string(metadata_version));
    if (!effective_profile.encoder.empty()) {
        cmd.push_back("--encoder");
        cmd.push_back(effective_profile.encoder);
    }
    for (const auto &tok : effective_profile.ffmpeg_opts) {
        cmd.push_back("--ffopt");
        cmd.push_back(tok);
    }
    if (profile_sel.forced_crf >= 0) {
        int forced_crf = profile_sel.forced_crf;
        const CrfBounds bounds = crf_bounds_for_profile(effective_profile);
        if (bounds.known) {
            if (forced_crf < bounds.min) forced_crf = bounds.min;
            if (forced_crf > bounds.max) forced_crf = bounds.max;
            if (forced_crf != profile_sel.forced_crf) {
                std::cerr << "[warn] requested profile suffix _crf" << profile_sel.forced_crf
                          << " is outside supported range [" << bounds.min << "," << bounds.max
                          << "] for profile '" << effective_profile.name
                          << "'; clamping to _crf" << forced_crf << "\n";
            }
        }
        cmd.push_back("--force-crf");
        cmd.push_back(std::to_string(forced_crf));
    }
    if (threads > 0) {
        cmd.push_back("--threads");
        cmd.push_back(std::to_string(threads));
    }
    cmd.push_back(headers_only_scan ? "--headers_only_scan" : "--no_headers_only_scan");
    cmd.push_back(fast_path ? "--fast-path" : "--no_fast_path");
    if (verbose) cmd.push_back("--verbose");
    for (const auto &in : inputs) cmd.push_back(in);

    if (avc_12bit_fallback) {
        std::cerr << "[warn] profile '" << profile.name
                  << "' requested AVC 12-bit, but local libx264 does not report 12-bit pixel format support. "
                  << "Falling back to AVC 10-bit.\n";
    }

    int ret = run_command(cmd);
    if (ret != 0) {
        std::cerr << "grib2avenc failed with code " << ret << "\n";
        return ret;
    }
    return 0;
}
