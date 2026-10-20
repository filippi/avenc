#include "avenc_codec.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace avenc {
namespace {

std::string escape_quotes(const std::string &input) {
    std::string result;
    result.reserve(input.size());
    for (char c : input) {
        if (c == '"') {
            result += "\\\"";
        } else {
            result += c;
        }
    }
    return result;
}

std::string build_encoder_comment(const EncodeOptions &options,
                                  size_t frames,
                                  size_t height,
                                  size_t width) {
    if (!options.comment_metadata.empty()) {
        return options.comment_metadata;
    }
    std::ostringstream oss;
    oss << "{\"Ni\":" << width
        << ",\"Nj\":" << height
        << ",\"frames\":" << frames
        << ",\"minValue\":" << options.min_value
        << ",\"maxValue\":" << options.max_value
        << ",\"bits\":" << options.bits
        << ",\"codec\":\"" << options.codec << "\""
        << ",\"quality\":\"" << options.quality << "\""
        << ",\"log2\":" << (options.apply_log2 ? "true" : "false")
        << ",\"withNan\":" << (options.allow_nan ? "true" : "false")
        << ",\"flipud\":" << (options.flip_ud ? "true" : "false")
        << "}";
    return oss.str();
}

std::string ffmpeg_codec_name(const std::string &codec) {
    if (codec == "avc" || codec == "h264") {
        return "libx264";
    }
    if (codec == "hevc" || codec == "h265") {
        return "libx265";
    }
    if (codec == "av1") {
        return "libaom-av1";
    }
    // Allow callers to pass an explicit encoder name (e.g. hevc_videotoolbox)
    return codec;
}

const char *input_pixel_format(int bits) {
    return (bits <= 8) ? "gray8" : "gray16le";
}

std::vector<uint8_t> base64_decode(const std::string &input) {
    static const int8_t decode_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::vector<uint8_t> output;
    output.reserve((input.size() * 3) / 4);
    int val = 0;
    int bits = -8;
    for (unsigned char c : input) {
        int8_t d = decode_table[c];
        if (d == -1) {
            continue;
        }
        if (d == -2) {
            break;
        }
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

} // namespace

std::vector<std::pair<std::string, std::string>> get_ffmpeg_encoder_options(const std::string &codec,
                                                                            const std::string &quality) {
    std::vector<std::pair<std::string, std::string>> options;

    if (codec == "ffv1") {
        if (quality == "lossless") {
            options.push_back({"-level", "3"});
            options.push_back({"-coder", "1"});
            options.push_back({"-context", "1"});
            options.push_back({"-g", "1"});
            options.push_back({"-slices", "24"});
        } else {
            throw std::invalid_argument("Unsupported ffv1 quality preset: " + quality);
        }
        return options;
    }

    if (quality == "lossless") {
        if (codec == "avc") {
            options.push_back({"-crf", "0"});
            options.push_back({"-preset", "veryslow"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "0"});
            options.push_back({"-preset", "veryslow"});
            options.push_back({"-x265-params", "lossless=1"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "0"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "veryslow"});
        } else {
            throw std::invalid_argument("Unsupported codec for lossless preset: " + codec);
        }
    } else if (quality == "veryhigh") {
        if (codec == "avc") {
            options.push_back({"-crf", "5"});
            options.push_back({"-preset", "slow"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "5"});
            options.push_back({"-x265-params", "profile=monochrome12:crf=5:preset=slow"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "5"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "slow"});
        } else {
            throw std::invalid_argument("Unsupported codec for veryhigh preset: " + codec);
        }
    } else if (quality == "high") {
        if (codec == "avc") {
            options.push_back({"-crf", "10"});
            options.push_back({"-bf", "3"});
            options.push_back({"-coder", "1"});
            options.push_back({"-preset", "medium"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "10"});
            options.push_back({"-x265-params", "profile=monochrome12:crf=10:preset=medium:ref=4"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "10"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "medium"});
        } else {
            throw std::invalid_argument("Unsupported codec for high preset: " + codec);
        }
    } else if (quality == "average") {
        if (codec == "avc") {
            options.push_back({"-crf", "20"});
            options.push_back({"-preset", "medium"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "20"});
            options.push_back({"-x265-params", "profile=main:crf=20:preset=medium"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "20"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "medium"});
        } else {
            throw std::invalid_argument("Unsupported codec for average preset: " + codec);
        }
    } else if (quality == "low") {
        if (codec == "avc") {
            options.push_back({"-crf", "28"});
            options.push_back({"-preset", "fast"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "28"});
            options.push_back({"-x265-params", "profile=main:crf=28:preset=fast"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "28"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "fast"});
        } else {
            throw std::invalid_argument("Unsupported codec for low preset: " + codec);
        }
    } else if (quality == "verylow") {
        if (codec == "avc") {
            options.push_back({"-crf", "35"});
            options.push_back({"-preset", "veryfast"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "35"});
            options.push_back({"-x265-params", "profile=main:crf=35:preset=veryfast"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "35"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "veryfast"});
        } else {
            throw std::invalid_argument("Unsupported codec for verylow preset: " + codec);
        }
    } else if (quality == "fast") {
        if (codec == "avc") {
            options.push_back({"-crf", "23"});
            options.push_back({"-preset", "ultrafast"});
            options.push_back({"-bf", "0"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "23"});
            options.push_back({"-x265-params", "profile=main:crf=23:preset=ultrafast"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "23"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "ultrafast"});
        } else {
            throw std::invalid_argument("Unsupported codec for fast preset: " + codec);
        }
    } else if (quality == "compatible") {
        options.push_back({"-crf", "20"});
        options.push_back({"-preset", "medium"});
    } else if (quality == "multiset") {
        if (codec == "avc") {
            options.push_back({"-crf", "15"});
            options.push_back({"-g", "12"});
            options.push_back({"-sc_threshold", "40"});
            options.push_back({"-preset", "medium"});
        } else if (codec == "hevc") {
            options.push_back({"-crf", "15"});
            options.push_back({"-g", "12"});
            options.push_back({"-x265-params", "profile=monochrome12:crf=15:preset=medium:open_gop=1"});
        } else if (codec == "av1") {
            options.push_back({"-crf", "15"});
            options.push_back({"-g", "12"});
            options.push_back({"-b:v", "0"});
            options.push_back({"-preset", "medium"});
        } else {
            throw std::invalid_argument("Unsupported codec for multiset preset: " + codec);
        }
    } else {
        throw std::invalid_argument("Unknown quality preset: " + quality);
    }

    return options;
}

EncodeResult encode_video_from_float_array(const float *data,
                                           size_t frames,
                                           size_t height,
                                           size_t width,
                                           const EncodeOptions &options,
                                           const std::string &output_path) {
    if (!data) {
        throw std::invalid_argument("encode_video_from_float_array: data pointer is null");
    }
    if (frames == 0 || height == 0 || width == 0) {
        throw std::invalid_argument("encode_video_from_float_array: invalid dimensions");
    }
    if (!(options.bits == 8 || options.bits == 10 || options.bits == 12 || options.bits == 16)) {
        throw std::invalid_argument("encode_video_from_float_array: bits must be 8, 10, 12, or 16");
    }
    if (options.max_value <= options.min_value) {
        throw std::invalid_argument("encode_video_from_float_array: max_value must be greater than min_value");
    }

    const uint32_t max_int = (1u << options.bits) - 1u;
    const double range = options.max_value - options.min_value;
    if (range <= 0.0) {
        throw std::invalid_argument("encode_video_from_float_array: invalid range");
    }

    std::filesystem::path out_path(output_path);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path());
    }

    std::string codec_name = options.encoder.empty() ? ffmpeg_codec_name(options.codec)
                                                     : options.encoder;
    std::string input_pix_fmt = options.pix_fmt.empty() ? input_pixel_format(options.bits)
                                                       : options.pix_fmt;
    std::string output_pix_fmt = options.output_pix_fmt;
    if (!options.color_mapping.empty()) {
        const size_t expected_size = (static_cast<size_t>(max_int) + 1) * 3;
        const auto color_map = base64_decode(options.color_mapping);
        if (!color_map.empty() && color_map.size() != expected_size) {
            throw std::invalid_argument("color_mapping size mismatch");
        }
    }
    auto encoder_options = get_ffmpeg_encoder_options(options.codec, options.quality);
    if (!options.encoder.empty()) {
        encoder_options.erase(
            std::remove_if(encoder_options.begin(), encoder_options.end(), [](const auto &opt) {
                return opt.first == "-pix_fmt" || opt.first == "-x265-params";
            }),
            encoder_options.end());
    }

    auto pix_fmt_for_bits = [](int bits) -> std::string {
        if (bits <= 8) {
            return "gray";
        }
        if (bits <= 10) {
            return "gray10le";
        }
        if (bits <= 12) {
            return "gray12le";
        }
        return "gray16le";
    };

    encoder_options.erase(
        std::remove_if(encoder_options.begin(), encoder_options.end(), [](const auto &opt) {
            return opt.first == "-pix_fmt";
        }),
        encoder_options.end());

    const std::string target_pix_fmt = output_pix_fmt.empty()
                                           ? pix_fmt_for_bits(options.bits)
                                           : output_pix_fmt;

    if (!target_pix_fmt.empty()) {
        encoder_options.emplace_back("-pix_fmt", target_pix_fmt);
    }

    std::ostringstream opts_str;
    for (const auto &opt : encoder_options) {
        opts_str << ' ' << opt.first << ' ' << opt.second;
    }

    const std::string comment = escape_quotes(build_encoder_comment(options, frames, height, width));

    std::ostringstream ffmpeg_cmd;
    ffmpeg_cmd << "ffmpeg -y -loglevel error"
               << " -f rawvideo"
               << " -pix_fmt " << input_pix_fmt
               << " -video_size " << width << 'x' << height
               << " -framerate " << options.frame_rate
               << " -i -"
               << " -c:v " << codec_name
               << opts_str.str()
               << " -metadata \"comment=" << comment << "\""
               << " \"" << output_path << "\"";

    FILE *pipe = popen(ffmpeg_cmd.str().c_str(), "w");
    if (!pipe) {
        throw std::runtime_error("Failed to launch ffmpeg: " + ffmpeg_cmd.str());
    }

    const size_t plane_size = height * width;
    const double inv_range = 1.0 / range;
    const double scale = static_cast<double>(max_int);
    const double log2precision = std::exp2(1.0 / static_cast<double>(max_int));
    const double precision_of_log2 = std::log2(log2precision);
    const double inv_log2_range = 1.0 / -precision_of_log2;

    std::vector<uint16_t> buffer16(plane_size, 0);
    std::vector<uint8_t> buffer8(plane_size, 0);

    for (size_t frame_idx = 0; frame_idx < frames; ++frame_idx) {
        const float *frame_ptr = data + frame_idx * plane_size;
        if (options.bits <= 8) {
            for (size_t i = 0; i < plane_size; ++i) {
                size_t src_index = i;
                if (options.flip_ud) {
                    size_t row = i / width;
                    size_t col = i % width;
                    src_index = (height - row - 1) * width + col;
                }
                float value = frame_ptr[src_index];
                if (std::isnan(value)) {
                    if (options.allow_nan) {
                        buffer8[i] = 0;
                        continue;
                    }
                    value = static_cast<float>(options.min_value);
                } else if (!std::isfinite(value)) {
                    value = (value > 0.f)
                                ? static_cast<float>(options.max_value)
                                : static_cast<float>(options.min_value);
                }
                value = std::clamp(value, static_cast<float>(options.min_value), static_cast<float>(options.max_value));
                double norm = (value - options.min_value) * inv_range;
                norm = std::clamp(norm, 0.0, 1.0);
                uint32_t quantized = 0;
                if (options.apply_log2) {
                    if (norm > 0.0) {
                        const double log_value = std::log2(norm);
                        quantized = static_cast<uint32_t>(std::round(log_value * inv_log2_range * scale));
                    }
                } else {
                    quantized = static_cast<uint32_t>(std::round(norm * scale));
                }
                if (quantized > max_int) {
                    quantized = max_int;
                }
                buffer8[i] = static_cast<uint8_t>(quantized);
            }
            size_t written = std::fwrite(buffer8.data(), sizeof(uint8_t), plane_size, pipe);
            if (written != plane_size) {
                pclose(pipe);
                throw std::runtime_error("Failed to write frame data to ffmpeg pipe (8-bit)");
            }
        } else {
            for (size_t i = 0; i < plane_size; ++i) {
                size_t src_index = i;
                if (options.flip_ud) {
                    size_t row = i / width;
                    size_t col = i % width;
                    src_index = (height - row - 1) * width + col;
                }
                float value = frame_ptr[src_index];
                if (std::isnan(value)) {
                    if (options.allow_nan) {
                        buffer16[i] = 0;
                        continue;
                    }
                    value = static_cast<float>(options.min_value);
                } else if (!std::isfinite(value)) {
                    value = (value > 0.f)
                                ? static_cast<float>(options.max_value)
                                : static_cast<float>(options.min_value);
                }
                value = std::clamp(value, static_cast<float>(options.min_value), static_cast<float>(options.max_value));
                double norm = (value - options.min_value) * inv_range;
                norm = std::clamp(norm, 0.0, 1.0);
                uint32_t quantized = 0;
                if (options.apply_log2) {
                    if (norm > 0.0) {
                        const double log_value = std::log2(norm);
                        quantized = static_cast<uint32_t>(std::round(log_value * inv_log2_range * scale));
                    }
                } else {
                    quantized = static_cast<uint32_t>(std::round(norm * scale));
                }
                if (quantized > max_int) {
                    quantized = max_int;
                }
                buffer16[i] = static_cast<uint16_t>(quantized);
            }
            size_t written = std::fwrite(buffer16.data(), sizeof(uint16_t), plane_size, pipe);
            if (written != plane_size) {
                pclose(pipe);
                throw std::runtime_error("Failed to write frame data to ffmpeg pipe (16-bit)");
            }
        }
    }

    int ret = pclose(pipe);
    if (ret != 0) {
        throw std::runtime_error("ffmpeg exited with code " + std::to_string(ret));
    }

    return EncodeResult{frames};
}

std::vector<float> decode_video_to_float_array(const std::string &input_path,
                                               size_t frames,
                                               size_t height,
                                               size_t width,
                                               const DecodeOptions &options) {
    if (frames == 0 || height == 0 || width == 0) {
        throw std::invalid_argument("decode_video_to_float_array: invalid dimensions");
    }
    if (!(options.bits == 8 || options.bits == 10 || options.bits == 12 || options.bits == 16)) {
        throw std::invalid_argument("decode_video_to_float_array: bits must be 8, 10, 12, or 16");
    }
    if (options.max_value <= options.min_value) {
        throw std::invalid_argument("decode_video_to_float_array: max_value must be greater than min_value");
    }

    const uint32_t max_int = (1u << options.bits) - 1u;
    const double value_range = options.max_value - options.min_value;
    const size_t plane_size = height * width;
    const size_t total_values = frames * plane_size;
    std::string pix_fmt = options.pix_fmt.empty() ? input_pixel_format(options.bits)
                                                  : options.pix_fmt;
    bool use_color_mapping = !options.color_mapping.empty();
    std::vector<uint8_t> color_map;
    std::unordered_map<uint32_t, uint32_t> color_to_value;
    if (use_color_mapping) {
        color_map = base64_decode(options.color_mapping);
        const size_t expected = (static_cast<size_t>(max_int) + 1) * 3;
        if (color_map.size() != expected) {
            throw std::invalid_argument("color_mapping size mismatch");
        }
        pix_fmt = "rgb24";
        for (uint32_t v = 0; v <= max_int; ++v) {
            uint32_t packed = (static_cast<uint32_t>(color_map[v * 3]) << 16) |
                              (static_cast<uint32_t>(color_map[v * 3 + 1]) << 8) |
                              static_cast<uint32_t>(color_map[v * 3 + 2]);
            color_to_value[packed] = v;
        }
    }

    std::ostringstream ffmpeg_cmd;
    ffmpeg_cmd << "ffmpeg -loglevel error"
               << " -i \"" << input_path << "\""
               << " -map 0:v:" << options.stream_index
               << " -an -sn -vsync 0"
               << " -f rawvideo"
               << " -pix_fmt " << pix_fmt
               << " -";

    FILE *pipe = popen(ffmpeg_cmd.str().c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to launch ffmpeg for decode: " + ffmpeg_cmd.str());
    }

    size_t bytes_per_value = 0;
    if (use_color_mapping) {
        bytes_per_value = 3 * sizeof(uint8_t);
    } else {
        bytes_per_value = (options.bits <= 8) ? sizeof(uint8_t) : sizeof(uint16_t);
    }
    const size_t expected_bytes = total_values * bytes_per_value;

    std::vector<uint8_t> raw_bytes(expected_bytes);
    size_t total_read = 0;
    while (total_read < expected_bytes) {
        size_t chunk = std::fread(raw_bytes.data() + total_read, 1, expected_bytes - total_read, pipe);
        if (chunk == 0) {
            if (std::feof(pipe)) {
                break;
            }
            if (std::ferror(pipe)) {
                int err = errno;
                pclose(pipe);
                throw std::runtime_error("Error reading data from ffmpeg: " + std::string(std::strerror(err)));
            }
        }
        total_read += chunk;
    }

    int ret = pclose(pipe);
    if (ret != 0) {
        throw std::runtime_error("ffmpeg decode exited with code " + std::to_string(ret));
    }

    if (total_read != expected_bytes) {
        throw std::runtime_error("Unexpected amount of data read from ffmpeg. Expected " +
                                 std::to_string(expected_bytes) + " bytes, got " + std::to_string(total_read));
    }

    std::vector<float> decoded(total_values, std::numeric_limits<float>::quiet_NaN());
    const double inv_scale = 1.0 / static_cast<double>(max_int ? max_int : 1);
    const double log2precision = std::exp2(1.0 / static_cast<double>(max_int ? max_int : 1));
    const double precision_of_log2 = std::log2(log2precision);

    auto convert_quantized = [&](uint32_t q) -> float {
        if (options.allow_nan && q == 0) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        double norm = static_cast<double>(q) * inv_scale;
        if (options.apply_log2) {
            if (norm <= precision_of_log2) {
                norm = 0.0;
            } else {
                norm = std::pow(2.0, norm * log2precision);
            }
        }
        double value = norm * value_range + options.min_value;
        return static_cast<float>(value);
    };

    if (use_color_mapping) {
        const uint8_t *values = raw_bytes.data();
        for (size_t idx = 0; idx < total_values; ++idx) {
            uint32_t packed = (static_cast<uint32_t>(values[idx * 3]) << 16) |
                              (static_cast<uint32_t>(values[idx * 3 + 1]) << 8) |
                              static_cast<uint32_t>(values[idx * 3 + 2]);
            uint32_t q = 0;
            auto it = color_to_value.find(packed);
            if (it != color_to_value.end()) {
                q = it->second;
            } else {
                q = static_cast<uint32_t>(std::round((packed / 16777215.0) * max_int));
            }
            if (q > max_int) {
                q = max_int;
            }
            decoded[idx] = convert_quantized(q);
        }
    } else if (options.bits <= 8) {
        const uint8_t *values = raw_bytes.data();
        for (size_t idx = 0; idx < total_values; ++idx) {
            uint32_t q = values[idx];
            decoded[idx] = convert_quantized(q);
        }
    } else {
        const uint16_t *values = reinterpret_cast<const uint16_t *>(raw_bytes.data());
        for (size_t idx = 0; idx < total_values; ++idx) {
            uint32_t q = values[idx];
            decoded[idx] = convert_quantized(q);
        }
    }

    return decoded;
}

std::vector<float> decode_video_subset_to_float_array(const std::string &input_path,
                                                      size_t t0, size_t t1,
                                                      size_t j0, size_t j1,
                                                      size_t i0, size_t i1,
                                                      const DecodeOptions &options) {
    if (!(options.bits == 8 || options.bits == 10 || options.bits == 12 || options.bits == 16)) {
        throw std::invalid_argument("decode_video_subset_to_float_array: bits must be 8, 10, 12, or 16");
    }
    if (options.max_value <= options.min_value) {
        throw std::invalid_argument("decode_video_subset_to_float_array: max_value must be greater than min_value");
    }

    if (!(t1 >= t0 && j1 >= j0 && i1 >= i0)) {
        return {};
    }
    const size_t out_frames = t1 - t0;
    const size_t out_h = j1 - j0;
    const size_t out_w = i1 - i0;
    // Some ffmpeg builds/filters choke on crop=1:1; request at least 2x2,
    // then crop back in-memory.
    const size_t ff_h = std::max<size_t>(2, out_h);
    const size_t ff_w = std::max<size_t>(2, out_w);

    const uint32_t max_int = (1u << options.bits) - 1u;
    const double value_range = options.max_value - options.min_value;
    const size_t ff_plane_size = ff_h * ff_w;
    const size_t ff_total_values = out_frames * ff_plane_size;
    std::string pix_fmt = options.pix_fmt.empty() ? input_pixel_format(options.bits)
                                                  : options.pix_fmt;
    bool use_color_mapping = !options.color_mapping.empty();
    std::vector<uint8_t> color_map;
    std::unordered_map<uint32_t, uint32_t> color_to_value;
    if (use_color_mapping) {
        color_map = base64_decode(options.color_mapping);
        const size_t expected = (static_cast<size_t>(max_int) + 1) * 3;
        if (color_map.size() != expected) {
            throw std::invalid_argument("color_mapping size mismatch");
        }
        pix_fmt = "rgb24";
        for (uint32_t v = 0; v <= max_int; ++v) {
            uint32_t packed = (static_cast<uint32_t>(color_map[v * 3]) << 16) |
                              (static_cast<uint32_t>(color_map[v * 3 + 1]) << 8) |
                              static_cast<uint32_t>(color_map[v * 3 + 2]);
            color_to_value[packed] = v;
        }
    }

    std::ostringstream vf;
    const size_t end_inclusive = (t1 == 0) ? 0 : (t1 - 1);
    vf << "select='between(n," << t0 << "," << end_inclusive << ")',setpts=N/FRAME_RATE/TB";
    vf << ",crop=" << ff_w << ':' << ff_h << ':' << i0 << ':' << j0 << ":exact=1";

    std::ostringstream ffmpeg_cmd;
    ffmpeg_cmd << "ffmpeg -loglevel error"
               << " -i \"" << input_path << "\""
               << " -map 0:v:" << options.stream_index << " -an -sn -vsync 0"
               << " -vf \"" << vf.str() << "\""
               << " -f rawvideo"
               << " -pix_fmt " << pix_fmt
               << " -";

    FILE *pipe = popen(ffmpeg_cmd.str().c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to launch ffmpeg for subset decode: " + ffmpeg_cmd.str());
    }

    size_t bytes_per_value = 0;
    if (use_color_mapping) {
        bytes_per_value = 3 * sizeof(uint8_t);
    } else {
        bytes_per_value = (options.bits <= 8) ? sizeof(uint8_t) : sizeof(uint16_t);
    }
    const size_t expected_bytes = ff_total_values * bytes_per_value;

    std::vector<uint8_t> raw_bytes(expected_bytes);
    size_t total_read = 0;
    while (total_read < expected_bytes) {
        size_t chunk = std::fread(raw_bytes.data() + total_read, 1, expected_bytes - total_read, pipe);
        if (chunk == 0) {
            if (std::feof(pipe)) {
                break;
            }
            if (std::ferror(pipe)) {
                int err = errno;
                pclose(pipe);
                throw std::runtime_error("Error reading subset data from ffmpeg: " + std::string(std::strerror(err)));
            }
        }
        total_read += chunk;
    }

    int ret = pclose(pipe);
    if (ret != 0) {
        throw std::runtime_error("ffmpeg subset decode exited with code " + std::to_string(ret));
    }

    if (total_read != expected_bytes) {
        throw std::runtime_error("Unexpected subset size from ffmpeg. Expected " +
                                 std::to_string(expected_bytes) + " bytes, got " + std::to_string(total_read));
    }

    std::vector<float> decoded(ff_total_values, std::numeric_limits<float>::quiet_NaN());
    const double inv_scale = 1.0 / static_cast<double>(max_int);
    const double log2precision = std::exp2(1.0 / static_cast<double>(max_int));
    const double precision_of_log2 = std::log2(log2precision);
    auto convert_quantized = [&](uint32_t q) -> float {
        if (options.allow_nan && q == 0) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        double norm = static_cast<double>(q) * inv_scale;
        if (options.apply_log2) {
            if (norm <= precision_of_log2) {
                norm = 0.0;
            } else {
                norm = std::pow(2.0, norm * log2precision);
            }
        }
        double value = norm * value_range + options.min_value;
        return static_cast<float>(value);
    };

    if (use_color_mapping) {
        const uint8_t *values = raw_bytes.data();
        for (size_t idx = 0; idx < ff_total_values; ++idx) {
            uint32_t packed = (static_cast<uint32_t>(values[idx * 3]) << 16) |
                              (static_cast<uint32_t>(values[idx * 3 + 1]) << 8) |
                              static_cast<uint32_t>(values[idx * 3 + 2]);
            uint32_t q = 0;
            auto it = color_to_value.find(packed);
            if (it != color_to_value.end()) {
                q = it->second;
            } else {
                q = static_cast<uint32_t>(std::round((packed / 16777215.0) * max_int));
            }
            if (q > max_int) {
                q = max_int;
            }
            decoded[idx] = convert_quantized(q);
        }
    } else if (options.bits <= 8) {
        const uint8_t *values = raw_bytes.data();
        for (size_t idx = 0; idx < ff_total_values; ++idx) {
            uint32_t q = values[idx];
            decoded[idx] = convert_quantized(q);
        }
    } else {
        const uint16_t *values = reinterpret_cast<const uint16_t *>(raw_bytes.data());
        for (size_t idx = 0; idx < ff_total_values; ++idx) {
            uint32_t q = values[idx];
            decoded[idx] = convert_quantized(q);
        }
    }

    // If we requested a larger crop for ffmpeg (2x2) but the user asked for 1x1
    // or 1xN, slice back to the requested shape.
    if (ff_h != out_h || ff_w != out_w) {
        std::vector<float> sliced(out_frames * out_h * out_w);
        for (size_t f = 0; f < out_frames; ++f) {
            for (size_t j = 0; j < out_h; ++j) {
                const float *src = &decoded[f * (ff_h * ff_w) + j * ff_w];
                float *dst = &sliced[f * (out_h * out_w) + j * out_w];
                std::memcpy(dst, src, out_w * sizeof(float));
            }
        }
        return sliced;
    }
    return decoded;
}

} // namespace avenc
