#include "avenc_transform.h"
// Packed-GRIB AVENC v1/v2 metadata reader, adapted from avenc2grib.cpp.
// Deliberately isolated from metric calculation; scientific values are decoded
// through the existing avenc_subset_decode library, never repacked to GRIB.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>
#include <zlib.h>
namespace avenc_compare_metadata {
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

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
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
    uint32_t val = 0;
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

std::string shell_quote(const std::string &s) {
    std::string out="'";
    for(char c:s) { if(c=='\'') out+="'\\''"; else out+=c; }
    return out+"'";
}
std::string read_command_stdout(const std::vector<std::string>& args) {
    std::string command;
    for(const auto& a:args) command += (command.empty()?"":" ")+shell_quote(a);
    FILE *p=popen(command.c_str(),"r");
    if(!p) throw std::runtime_error("Cannot launch: "+command);
    std::string result; char buf[8192]; size_t n;
    while((n=fread(buf,1,sizeof(buf),p))) result.append(buf,n);
    bool failed=ferror(p); int rc=pclose(p);
    if(failed || rc) throw std::runtime_error("Command failed: "+command);
    return result;
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


ReconPayload read(const std::string& path) {
    auto decoded=decode_avenc_comment(extract_comment(path));
    if(decoded.has_binary_recon) return std::move(decoded.recon_binary);
    if(decoded.recon_format!="avpack.recon.tsv.zlib.base64.v1")
        throw std::runtime_error("Unsupported packed GRIB reconstruction format: "+decoded.recon_format);
    auto bytes=zlib_decompress(base64_decode(decoded.recon_payload));
    return parse_recon_payload(std::string(bytes.begin(),bytes.end()));
}
} // namespace avenc_compare_metadata
