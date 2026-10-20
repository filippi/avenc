#include "avenc_codec.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <cstring>
#include <stdexcept>
#include <vector>

namespace py = pybind11;

namespace {

avenc::EncodeOptions encode_options_from_dict(const py::dict &options) {
    avenc::EncodeOptions opts;
    if (options.contains("bits")) {
        opts.bits = options["bits"].cast<int>();
    }
    if (options.contains("min_value")) {
        opts.min_value = options["min_value"].cast<double>();
    }
    if (options.contains("max_value")) {
        opts.max_value = options["max_value"].cast<double>();
    }
    if (options.contains("apply_log2")) {
        opts.apply_log2 = options["apply_log2"].cast<bool>();
    }
    if (options.contains("flip_ud")) {
        opts.flip_ud = options["flip_ud"].cast<bool>();
    }
    if (options.contains("allow_nan")) {
        opts.allow_nan = options["allow_nan"].cast<bool>();
    }
    if (options.contains("codec")) {
        opts.codec = options["codec"].cast<std::string>();
    }
    if (options.contains("encoder")) {
        opts.encoder = options["encoder"].cast<std::string>();
    }
    if (options.contains("quality")) {
        opts.quality = options["quality"].cast<std::string>();
    }
    if (options.contains("frame_rate")) {
        opts.frame_rate = options["frame_rate"].cast<int>();
    }
    if (options.contains("comment_metadata")) {
        opts.comment_metadata = options["comment_metadata"].cast<std::string>();
    }
    if (options.contains("pix_fmt")) {
        opts.pix_fmt = options["pix_fmt"].cast<std::string>();
    }
    if (options.contains("output_pix_fmt")) {
        opts.output_pix_fmt = options["output_pix_fmt"].cast<std::string>();
    }
    if (options.contains("color_mapping")) {
        opts.color_mapping = options["color_mapping"].cast<std::string>();
    }
    return opts;
}

avenc::DecodeOptions decode_options_from_dict(const py::dict &options) {
    avenc::DecodeOptions opts;
    if (options.contains("bits")) {
        opts.bits = options["bits"].cast<int>();
    }
    if (options.contains("min_value")) {
        opts.min_value = options["min_value"].cast<double>();
    }
    if (options.contains("max_value")) {
        opts.max_value = options["max_value"].cast<double>();
    }
    if (options.contains("apply_log2")) {
        opts.apply_log2 = options["apply_log2"].cast<bool>();
    }
    if (options.contains("allow_nan")) {
        opts.allow_nan = options["allow_nan"].cast<bool>();
    }
    if (options.contains("frame_rate")) {
        opts.frame_rate = options["frame_rate"].cast<int>();
    }
    if (options.contains("stream_index")) {
        opts.stream_index = options["stream_index"].cast<int>();
    }
    if (options.contains("pix_fmt")) {
        opts.pix_fmt = options["pix_fmt"].cast<std::string>();
    }
    if (options.contains("color_mapping")) {
        opts.color_mapping = options["color_mapping"].cast<std::string>();
    }
    return opts;
}

} // namespace

PYBIND11_MODULE(_avenc_ext, m) {
    m.doc() = "Pybind11 extension providing MetFlix-based encoding/decoding for xarray";

    m.def(
        "encode",
        [](py::array input_array, const std::string &output_path, const py::dict &options_dict) {
            py::array_t<float, py::array::c_style | py::array::forcecast> data(input_array);
            if (data.ndim() != 3) {
                throw std::invalid_argument("encode expects a 3D array with dimensions (time, y, x)");
            }
            const size_t frames = static_cast<size_t>(data.shape(0));
            const size_t height = static_cast<size_t>(data.shape(1));
            const size_t width  = static_cast<size_t>(data.shape(2));

            avenc::EncodeOptions options = encode_options_from_dict(options_dict);

            avenc::EncodeResult result;
            {
                py::gil_scoped_release release;
                result = avenc::encode_video_from_float_array(
                    static_cast<const float *>(data.data()),
                    frames,
                    height,
                    width,
                    options,
                    output_path);
            }
            return result.frames_written;
        },
        py::arg("data"), py::arg("output_path"), py::arg("options") = py::dict());

    m.def(
        "decode",
        [](const std::string &input_path,
           size_t frames,
           size_t height,
           size_t width,
           const py::dict &options_dict) {
            avenc::DecodeOptions options = decode_options_from_dict(options_dict);
            std::vector<float> decoded;
            {
                py::gil_scoped_release release;
                decoded = avenc::decode_video_to_float_array(
                    input_path,
                    frames,
                    height,
                    width,
                    options);
            }

            py::array_t<float> array({static_cast<py::ssize_t>(frames),
                                      static_cast<py::ssize_t>(height),
                                      static_cast<py::ssize_t>(width)});
            const size_t expected = frames * height * width;
            if (decoded.size() != expected) {
                throw std::runtime_error(
                    "decode returned unexpected number of values: expected " +
                    std::to_string(expected) + ", got " + std::to_string(decoded.size()));
            }
            if (!decoded.empty()) {
                std::memcpy(array.mutable_data(), decoded.data(), decoded.size() * sizeof(float));
            }
            return array;
        },
        py::arg("input_path"),
        py::arg("frames"),
        py::arg("height"),
        py::arg("width"),
        py::arg("options") = py::dict());

    m.def(
        "decode_subset",
        [](const std::string &input_path,
           size_t t0, size_t t1,
           size_t j0, size_t j1,
           size_t i0, size_t i1,
           const py::dict &options_dict) {
            avenc::DecodeOptions options = decode_options_from_dict(options_dict);
            std::vector<float> decoded;
            size_t frames = (t1 >= t0) ? (t1 - t0) : 0;
            size_t height = (j1 >= j0) ? (j1 - j0) : 0;
            size_t width  = (i1 >= i0) ? (i1 - i0) : 0;
            {
                py::gil_scoped_release release;
                decoded = avenc::decode_video_subset_to_float_array(
                    input_path, t0, t1, j0, j1, i0, i1, options);
            }
            const size_t expected = frames * height * width;
            if (decoded.size() != expected) {
                throw std::runtime_error(
                    "decode_subset returned unexpected number of values: expected " +
                    std::to_string(expected) + ", got " + std::to_string(decoded.size()));
            }
            py::array_t<float> array({static_cast<py::ssize_t>(frames),
                                      static_cast<py::ssize_t>(height),
                                      static_cast<py::ssize_t>(width)});
            if (!decoded.empty()) {
                std::memcpy(array.mutable_data(), decoded.data(), decoded.size() * sizeof(float));
            }
            return array;
        },
        py::arg("input_path"),
        py::arg("t0"), py::arg("t1"),
        py::arg("j0"), py::arg("j1"),
        py::arg("i0"), py::arg("i1"),
        py::arg("options") = py::dict());
}
