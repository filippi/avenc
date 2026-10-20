#include "avenc_subset_decode.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <zlib.h>

namespace {

struct StreamInfo {
    int stream_index = 0;
    int frame_count = 0;
    int bits = 8;
    int storage_shift = 0;
    double min_value = 0.0;
    double max_value = 1.0;
    bool apply_log2 = false;
    double log_offset = 1.0;
    std::vector<uint8_t> shared_bitmap;
    int encoded_nx = 0;
    int encoded_ny = 0;
    int crop_nx = 0;
    int crop_ny = 0;
};

struct RecordInfo {
    int stream_index = 0;
    int frame_index = 0;
    double frame_min = std::numeric_limits<double>::quiet_NaN();
    double frame_max = std::numeric_limits<double>::quiet_NaN();
    bool bitmap_present = false;
};

struct ReconPayload {
    int nx = 0;
    int ny = 0;
    std::vector<StreamInfo> streams;
    std::vector<RecordInfo> records;
};

thread_local std::string g_last_error;
thread_local std::string g_last_ffmpeg_cmd;

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
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
        const int d = table[c];
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
        const size_t have = chunk.size() - strm.avail_out;
        out.insert(out.end(), chunk.data(), chunk.data() + have);
    }
    inflateEnd(&strm);
    return out;
}

// Quote paths literally before passing commands to the shell.
std::string shell_quote(const std::string& s) {
    std::string out="'";
    for(char c:s) { if(c=='\'') out+="'\\''"; else out+=c; }
    return out+"'";
}

std::string read_command_stdout(const std::string &cmd) {
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("popen failed");

    std::string out;
    char buf[4096];
    while (true) {
        const size_t n = std::fread(buf, 1, sizeof(buf), pipe);
        if (n == 0) break;
        out.append(buf, n);
    }
    const int ret = pclose(pipe);
    if (ret != 0) throw std::runtime_error("Command failed: " + cmd);
    return out;
}

std::string extract_comment(const std::string &video) {
    std::ostringstream cmd;
    cmd << "ffprobe -v error"
        << " -show_entries format_tags=comment:stream_tags=comment"
        << " -of default=noprint_wrappers=1:nokey=1"
        << " " << shell_quote(video);
    const std::string out = read_command_stdout(cmd.str());
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string t = trim(line);
        if (t.rfind("AVENCv1|", 0) == 0 || t.rfind("AVENCv2|", 0) == 0) return t;
    }
    throw std::runtime_error("No AVENC metadata comment found in input");
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

struct DecodedComment {
    bool has_binary_recon = false;
    ReconPayload recon_binary;
    std::string metadata_json;
    std::string recon_format;
    std::string recon_payload;
};

ReconPayload parse_recon_payload_v2_binary(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 36) throw std::runtime_error("AVENCv2 payload too short");
    const char expect_magic[8] = {'A', 'V', '2', 'M', 'E', 'T', 'A', '\0'};
    if (std::memcmp(bytes.data(), expect_magic, 8) != 0) {
        throw std::runtime_error("Invalid AVENCv2 metadata magic");
    }
    const uint32_t fmt_ver = read_u32le(bytes.data() + 8);
    if (fmt_ver != 2u) throw std::runtime_error("Unsupported AVENCv2 format version (expected 2)");

    ReconPayload out;
    size_t off = 12;
    (void)read_u32le_at(bytes, off, "flags");
    out.nx = static_cast<int>(read_u32le_at(bytes, off, "nx"));
    out.ny = static_cast<int>(read_u32le_at(bytes, off, "ny"));
    const uint32_t stream_count = read_u32le_at(bytes, off, "stream_count");
    const uint32_t record_count = read_u32le_at(bytes, off, "record_count");
    (void)read_u32le_at(bytes, off, "reserved");

    out.streams.reserve(stream_count);
    for (uint32_t i = 0; i < stream_count; ++i) {
        StreamInfo s;
        s.stream_index = read_i32le_at(bytes, off, "stream_index");
        s.frame_count = static_cast<int>(read_u32le_at(bytes, off, "stream.frame_count"));
        s.bits = static_cast<int>(read_u32le_at(bytes, off, "stream.bits"));
        s.storage_shift = read_i32le_at(bytes, off, "stream.storage_shift");
        s.min_value = read_f64le_at(bytes, off, "stream.min_value");
        s.max_value = read_f64le_at(bytes, off, "stream.max_value");
        s.apply_log2 = (read_u32le_at(bytes, off, "stream.apply_log2") != 0u);
        s.log_offset = read_f64le_at(bytes, off, "stream.log_offset");
        s.encoded_nx = static_cast<int>(read_u32le_at(bytes, off, "stream.encoded_nx"));
        s.encoded_ny = static_cast<int>(read_u32le_at(bytes, off, "stream.encoded_ny"));
        s.crop_nx = static_cast<int>(read_u32le_at(bytes, off, "stream.crop_nx"));
        s.crop_ny = static_cast<int>(read_u32le_at(bytes, off, "stream.crop_ny"));
        const std::string bitmap_payload = read_string_u32le_at(bytes, off, "stream.bitmap_payload");
        (void)read_string_u32le_at(bytes, off, "stream.template_b64"); // not needed here
        if (!bitmap_payload.empty()) {
            const auto bmp_zip = base64_decode(bitmap_payload);
            s.shared_bitmap = zlib_decompress(bmp_zip);
        }
        out.streams.push_back(std::move(s));
    }

    out.records.reserve(record_count);
    for (uint32_t i = 0; i < record_count; ++i) {
        RecordInfo r;
        (void)read_u64le_at(bytes, off, "record.order");
        r.stream_index = read_i32le_at(bytes, off, "record.stream_index");
        r.frame_index = static_cast<int>(read_u32le_at(bytes, off, "record.frame_index"));
        (void)read_i32le_at(bytes, off, "record.file_index");
        (void)read_u64le_at(bytes, off, "record.message_index");
        r.frame_min = read_f64le_at(bytes, off, "record.frame_min");
        r.frame_max = read_f64le_at(bytes, off, "record.frame_max");
        r.bitmap_present = (read_u32le_at(bytes, off, "record.bitmap_present") != 0u);
        (void)read_i32le_at(bytes, off, "record.bits_per_value");
        (void)read_i32le_at(bytes, off, "record.binary_scale_shift");
        (void)read_string_u32le_at(bytes, off, "record.short_name");
        (void)read_string_u32le_at(bytes, off, "record.type_of_level");
        (void)read_string_u32le_at(bytes, off, "record.level");
        (void)read_string_u32le_at(bytes, off, "record.data_date");
        (void)read_string_u32le_at(bytes, off, "record.data_time");
        (void)read_string_u32le_at(bytes, off, "record.step_range");
        (void)read_string_u32le_at(bytes, off, "record.timestamp");
        (void)read_string_u32le_at(bytes, off, "record.ensemble_number");
        (void)read_string_u32le_at(bytes, off, "record.perturbation_number");
        (void)read_string_u32le_at(bytes, off, "record.template_b64");
        out.records.push_back(std::move(r));
    }

    if (out.nx <= 0 || out.ny <= 0) throw std::runtime_error("Missing/invalid GRID in AVENCv2 metadata");
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
    if (comment.rfind("AVENCv1|", 0) == 0) {
        const std::string meta_b64 = comment.substr(std::strlen("AVENCv1|"));
        const auto meta_zip = base64_decode(meta_b64);
        const auto meta_json_bytes = zlib_decompress(meta_zip);
        out.metadata_json.assign(meta_json_bytes.begin(), meta_json_bytes.end());
        out.recon_format = extract_json_string_field(out.metadata_json, "reconstruct_format");
        out.recon_payload = extract_json_string_field(out.metadata_json, "reconstruct_payload");
        return out;
    }
    if (comment.rfind("AVENCv2|", 0) == 0) {
        const std::string meta_b64 = comment.substr(std::strlen("AVENCv2|"));
        const auto meta_zip = base64_decode(meta_b64);
        const auto bytes = zlib_decompress(meta_zip);
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
        const std::string t = trim(line);
        if (t.empty()) continue;
        const auto cols = split_tab(t);
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
            s.frame_count = std::stoi(cols[2]);
            s.bits = std::stoi(cols[3]);
            s.min_value = std::stod(cols[4]);
            s.max_value = std::stod(cols[5]);
            if (cols.size() >= 7) s.storage_shift = std::stoi(cols[6]);
            if (cols.size() >= 8) {
                const std::string mode = trim(cols[7]);
                s.apply_log2 = (mode == "log2p1" || mode == "log2");
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
                    const auto bmp_zip = base64_decode(bitmap_payload);
                    s.shared_bitmap = zlib_decompress(bmp_zip);
                }
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
            out.streams.push_back(std::move(s));
            continue;
        }

        if (cols[0] == "RECORD") {
            if (cols.size() < 19) throw std::runtime_error("Invalid RECORD row");
            RecordInfo r;
            r.stream_index = std::stoi(cols[2]);
            r.frame_index = std::stoi(cols[3]);
            r.frame_min = std::stod(cols[15]);
            r.frame_max = std::stod(cols[16]);
            r.bitmap_present = (cols[17] == "1");
            out.records.push_back(std::move(r));
            continue;
        }
    }

    if (out.nx <= 0 || out.ny <= 0) throw std::runtime_error("Missing/invalid GRID in metadata");
    if (out.streams.empty()) throw std::runtime_error("No STREAM rows in metadata");
    if (out.records.empty()) throw std::runtime_error("No RECORD rows in metadata");
    for (auto &s : out.streams) {
        if (s.encoded_nx <= 0) s.encoded_nx = out.nx;
        if (s.encoded_ny <= 0) s.encoded_ny = out.ny;
        if (s.crop_nx <= 0) s.crop_nx = out.nx;
        if (s.crop_ny <= 0) s.crop_ny = out.ny;
    }
    return out;
}

const StreamInfo &find_stream(const ReconPayload &recon, int stream_id) {
    for (const auto &s : recon.streams) {
        if (s.stream_index == stream_id) return s;
    }
    throw std::runtime_error("Stream id not found in metadata: " + std::to_string(stream_id));
}

std::string build_subset_ffmpeg_command(const std::string &input_path,
                                        int stream_id,
                                        int bits,
                                        int ni_start,
                                        int nj_start,
                                        int width,
                                        int height,
                                        int frame_start,
                                        int frame_count) {
    const int frame_end = frame_start + frame_count - 1;
    const std::string pix_fmt = (bits <= 8) ? "gray" : "gray16le";
    std::ostringstream vf;
    vf << "select='between(n\\," << frame_start << "\\," << frame_end << ")'"
       << ",crop=" << width << ":" << height << ":" << ni_start << ":" << nj_start << ":exact=1"
       << ",setpts=N/FRAME_RATE/TB";

    std::ostringstream cmd;
    cmd << "ffmpeg -loglevel error"
        << " -i " << shell_quote(input_path)
        << " -map 0:v:" << stream_id
        << " -an -sn -fps_mode passthrough"
        << " -vf \"" << vf.str() << "\""
        << " -frames:v " << frame_count
        << " -f rawvideo -pix_fmt " << pix_fmt
        << " -";
    return cmd.str();
}

} // namespace

extern "C" int avenc_decode_subset_to_float32(
    const char *input_path,
    int stream_id,
    int ni_start, int nj_start, int width, int height,
    int frame_start, int frame_count,
    float **out_data,
    int *out_width, int *out_height,
    int *out_frame_count) {
    g_last_error.clear();
    g_last_ffmpeg_cmd.clear();

    if (!out_data || !out_width || !out_height || !out_frame_count) {
        g_last_error = "Output pointers must not be null";
        return 2;
    }
    *out_data = nullptr;
    *out_width = 0;
    *out_height = 0;
    *out_frame_count = 0;

    try {
        if (!input_path || !*input_path) throw std::runtime_error("input_path is empty");
        if (stream_id < 0) throw std::runtime_error("stream_id must be >= 0");
        if (ni_start < 0 || nj_start < 0) throw std::runtime_error("ni_start/nj_start must be >= 0");
        if (frame_start < 0) throw std::runtime_error("frame_start must be >= 0");

        const std::string comment = extract_comment(input_path);
        const DecodedComment decoded = decode_avenc_comment(comment);
        ReconPayload recon;
        if (decoded.has_binary_recon) {
            recon = decoded.recon_binary;
        } else {
            const std::string &recon_format = decoded.recon_format;
            if (recon_format != "avpack.recon.tsv.zlib.base64.v1") {
                throw std::runtime_error("Unsupported reconstruct_format: " + recon_format);
            }
            const std::string &recon_b64 = decoded.recon_payload;
            const auto recon_zip = base64_decode(recon_b64);
            const auto recon_text_bytes = zlib_decompress(recon_zip);
            const std::string recon_text(recon_text_bytes.begin(), recon_text_bytes.end());
            recon = parse_recon_payload(recon_text);
        }
        const StreamInfo &stream = find_stream(recon, stream_id);

        const int full_w = stream.crop_nx;
        const int full_h = stream.crop_ny;
        if (full_w <= 0 || full_h <= 0) throw std::runtime_error("Invalid stream geometry");
        if (ni_start >= full_w || nj_start >= full_h) {
            throw std::runtime_error("Crop origin is outside stream dimensions");
        }

        const int use_w = (width > 0) ? width : (full_w - ni_start);
        const int use_h = (height > 0) ? height : (full_h - nj_start);
        if (use_w <= 0 || use_h <= 0) throw std::runtime_error("Invalid crop width/height");
        if (ni_start + use_w > full_w || nj_start + use_h > full_h) {
            throw std::runtime_error("Crop rectangle exceeds stream dimensions");
        }
        if (frame_start >= stream.frame_count) {
            throw std::runtime_error("frame_start is outside stream frame range");
        }
        const int max_remaining = stream.frame_count - frame_start;
        const int use_frames = (frame_count > 0) ? frame_count : max_remaining;
        if (use_frames <= 0) throw std::runtime_error("Invalid frame_count");
        if (use_frames > max_remaining) throw std::runtime_error("Requested frame range exceeds stream");

        // Build per-frame metadata for fast lookup.
        std::vector<double> frame_min(static_cast<size_t>(stream.frame_count), stream.min_value);
        std::vector<double> frame_max(static_cast<size_t>(stream.frame_count), stream.max_value);
        std::vector<uint8_t> frame_bitmap(static_cast<size_t>(stream.frame_count), 0u);
        for (const auto &r : recon.records) {
            if (r.stream_index != stream.stream_index) continue;
            if (r.frame_index < 0 || r.frame_index >= stream.frame_count) continue;
            const size_t fi = static_cast<size_t>(r.frame_index);
            if (std::isfinite(r.frame_min)) frame_min[fi] = r.frame_min;
            if (std::isfinite(r.frame_max)) frame_max[fi] = r.frame_max;
            frame_bitmap[fi] = r.bitmap_present ? 1u : 0u;
        }

        const std::string ffmpeg_cmd = build_subset_ffmpeg_command(
            input_path, stream.stream_index, stream.bits,
            ni_start, nj_start, use_w, use_h, frame_start, use_frames);
        g_last_ffmpeg_cmd = ffmpeg_cmd;

        FILE *pipe = popen(ffmpeg_cmd.c_str(), "r");
        if (!pipe) throw std::runtime_error("Failed to launch ffmpeg decode pipe");

        const size_t frame_pixels = static_cast<size_t>(use_w) * static_cast<size_t>(use_h);
        const size_t total_values = static_cast<size_t>(use_frames) * frame_pixels;
        const size_t bytes_per_px = (stream.bits <= 8) ? 1u : 2u;
        const size_t expected_bytes = total_values * bytes_per_px;
        std::vector<uint8_t> raw(expected_bytes, 0u);
        size_t total_read = 0;
        while (total_read < expected_bytes) {
            const size_t n = std::fread(raw.data() + total_read, 1, expected_bytes - total_read, pipe);
            if (n == 0) break;
            total_read += n;
        }

        const int rc = pclose(pipe);
        if (rc != 0) throw std::runtime_error("ffmpeg subset decode command failed");
        if (total_read != expected_bytes) {
            std::ostringstream oss;
            oss << "Short read from ffmpeg: expected " << expected_bytes
                << " bytes, got " << total_read;
            throw std::runtime_error(oss.str());
        }

        std::vector<uint16_t> q(total_values, 0u);
        if (stream.bits <= 8) {
            for (size_t i = 0; i < total_values; ++i) q[i] = static_cast<uint16_t>(raw[i]);
        } else {
            for (size_t i = 0; i < total_values; ++i) {
                q[i] = static_cast<uint16_t>(
                    static_cast<uint16_t>(raw[2 * i]) |
                    (static_cast<uint16_t>(raw[2 * i + 1]) << 8));
            }
        }

        const bool has_shared_bitmap =
            (stream.shared_bitmap.size() == static_cast<size_t>(full_w) * static_cast<size_t>(full_h));
        const uint32_t qmax = (stream.bits >= 31) ? 0xFFFFFFFFu : ((1u << stream.bits) - 1u);

        std::vector<float> out(total_values, std::numeric_limits<float>::quiet_NaN());
        for (int t = 0; t < use_frames; ++t) {
            const int frame_idx = frame_start + t;
            double fmin = frame_min[static_cast<size_t>(frame_idx)];
            double fmax = frame_max[static_cast<size_t>(frame_idx)];
            if (!std::isfinite(fmin) || !std::isfinite(fmax) || fmax <= fmin) {
                fmin = stream.min_value;
                fmax = stream.max_value;
            }
            double span = fmax - fmin;
            if (!(span > 0.0)) {
                span = std::max(1e-12, std::fabs(fmin) * 1e-12);
            }
            const bool has_bitmap = has_shared_bitmap || (frame_bitmap[static_cast<size_t>(frame_idx)] != 0u);
            const uint32_t qvalid = has_bitmap ? (qmax > 0 ? qmax - 1u : 0u) : qmax;
            if (qvalid == 0u) throw std::runtime_error("Invalid quantization denominator");
            const double step = span / static_cast<double>(qvalid);
            const double d = 0.0;
            const double r = fmin * std::pow(10.0, d);
            const double eprime = std::log2(std::max(step * std::pow(10.0, d), 1e-30));
            const double scale2 = std::pow(2.0, eprime);
            const size_t frame_off = static_cast<size_t>(t) * frame_pixels;

            for (int j = 0; j < use_h; ++j) {
                for (int i = 0; i < use_w; ++i) {
                    const size_t idx = frame_off + static_cast<size_t>(j) * static_cast<size_t>(use_w) +
                                       static_cast<size_t>(i);
                    if (has_shared_bitmap) {
                        const size_t gidx =
                            static_cast<size_t>(nj_start + j) * static_cast<size_t>(full_w) +
                            static_cast<size_t>(ni_start + i);
                        if (gidx < stream.shared_bitmap.size() && stream.shared_bitmap[gidx] != 0u) {
                            out[idx] = std::numeric_limits<float>::quiet_NaN();
                            continue;
                        }
                    }

                    uint32_t qv = static_cast<uint32_t>(q[idx]);
                    if (has_bitmap && !has_shared_bitmap && qv == 0u) {
                        out[idx] = std::numeric_limits<float>::quiet_NaN();
                        continue;
                    }
                    if (stream.storage_shift > 0) {
                        const uint32_t round = 1u << static_cast<uint32_t>(stream.storage_shift - 1);
                        qv = (qv + round) >> static_cast<uint32_t>(stream.storage_shift);
                    }
                    if (qv > qmax) qv = qmax;

                    uint32_t x = qv;
                    if (has_bitmap) {
                        if (x <= 1u) x = 0u;
                        else x -= 1u;
                    }
                    const double y_linear = (r + static_cast<double>(x) * scale2) / std::pow(10.0, d);
                    double y_physical = y_linear;
                    if (stream.apply_log2) {
                        y_physical = std::exp2(y_linear) - stream.log_offset;
                    }
                    out[idx] = static_cast<float>(y_physical);
                }
            }
        }

        float *buf = static_cast<float *>(std::malloc(out.size() * sizeof(float)));
        if (!buf) throw std::runtime_error("malloc failed for output buffer");
        if (!out.empty()) {
            std::memcpy(buf, out.data(), out.size() * sizeof(float));
        }

        *out_data = buf;
        *out_width = use_w;
        *out_height = use_h;
        *out_frame_count = use_frames;
        return 0;
    } catch (const std::exception &ex) {
        g_last_error = ex.what();
        return 1;
    }
}

extern "C" void avenc_free_float32(float *ptr) {
    std::free(ptr);
}

extern "C" const char *avenc_decode_subset_last_error(void) {
    return g_last_error.c_str();
}

extern "C" const char *avenc_decode_subset_last_ffmpeg_command(void) {
    return g_last_ffmpeg_cmd.c_str();
}
