#pragma once
#include <string>

// AVENC metadata records whether integer codes occupy native low bits (shift=0)
// or the high bits of a gray16 buffer. Request the same layout on reconstruction
// so FFmpeg does not rescale a native 10/12-bit plane through 16-bit full range.
inline std::string avenc_raw_gray_format(int bits, int storage_shift) {
    if (bits <= 8) return "gray";
    if (storage_shift == 0 && (bits == 10 || bits == 12))
        return "gray" + std::to_string(bits) + "le";
    return "gray16le";
}
