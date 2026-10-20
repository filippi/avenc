#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace avenc {

struct EncodeOptions {
    int bits = 12;
    double min_value = 0.0;
    double max_value = 1.0;
    bool apply_log2 = false;
    bool flip_ud = false;
    bool allow_nan = false;
    std::string codec = "hevc";
    std::string quality = "high";
    std::string encoder; // optional explicit ffmpeg encoder name
    int frame_rate = 24;
    std::string comment_metadata; // optional JSON snippet that will be attached to ffmpeg output
    std::string pix_fmt;
    std::string color_mapping; // base64 mapping for color encoding
    std::string output_pix_fmt;
};

struct DecodeOptions {
    int bits = 12;
    double min_value = 0.0;
    double max_value = 1.0;
    bool apply_log2 = false;
    bool allow_nan = false;
    int frame_rate = 24;
    int stream_index = 0; // select input stream 0:v:<stream_index>
    std::string pix_fmt;
    std::string color_mapping;
};

struct EncodeResult {
    size_t frames_written = 0;
};

std::vector<std::pair<std::string, std::string>> get_ffmpeg_encoder_options(const std::string &codec,
                                                                            const std::string &quality);

EncodeResult encode_video_from_float_array(const float *data,
                                           size_t frames,
                                           size_t height,
                                           size_t width,
                                           const EncodeOptions &options,
                                           const std::string &output_path);

std::vector<float> decode_video_to_float_array(const std::string &input_path,
                                               size_t frames,
                                               size_t height,
                                               size_t width,
                                               const DecodeOptions &options);

// Decode a spatiotemporal subset using ffmpeg select+crop and map to float32.
// The subset is defined with Python slicing semantics [t0:t1), [j0:j1), [i0:i1).
std::vector<float> decode_video_subset_to_float_array(const std::string &input_path,
                                                      size_t t0, size_t t1,
                                                      size_t j0, size_t j1,
                                                      size_t i0, size_t i1,
                                                      const DecodeOptions &options);

} // namespace avenc
