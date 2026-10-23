#pragma once
#include <cmath>
#include <stdexcept>

namespace avenc_transform {
inline unsigned mode(bool log, bool asinh, bool shifted, double scale) {
    return shifted ? 4u : asinh ? (scale == 1.0 ? 2u : 3u) : log ? 1u : 0u;
}
inline void validate(double scale, double origin, double span) {
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(origin) ||
        !std::isfinite(span) || span <= 0)
        throw std::runtime_error("Invalid AVENC transform scale/origin/span");
}
inline double forward(double x, bool log, bool asinh, double log_offset,
                      bool shifted=false, double scale=1, double origin=0, double span=1) {
    if (!std::isfinite(x)) return x;
    if (shifted) {
        double z=(x-origin)/span;
        if (scale*z <= -1) throw std::runtime_error("shifted-log input outside transform domain");
        return std::log1p(scale*z)/std::log1p(scale);
    }
    if (asinh) return std::asinh(x/scale);
    if (!log) return x;
    if (x < 0) throw std::runtime_error("--log requires nonnegative valid values; use --shifted-log or --asinh for signed fields");
    return std::log1p(x/log_offset)/std::log(2.0);
}
inline double inverse(double y, bool log, bool asinh, double log_offset,
                      bool shifted=false, double scale=1, double origin=0, double span=1) {
    if (shifted) return origin+span*(std::expm1(y*std::log1p(scale))/scale);
    if (asinh) return scale*std::sinh(y);
    // Preserve the original log2p1 reconstruction for legacy metadata.
    return log ? std::exp2(y)-log_offset : y;
}
}
