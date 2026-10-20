#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Decode a spatio-temporal subset from one AVENC stream and reconstruct
// physical Float32 values.
//
// Output layout is frame-major, row-major within each frame:
//   index = ((t * out_height) + j) * out_width + i
//
// Bounds semantics:
// - if width <= 0 or height <= 0: use full remaining extent from (ni_start,nj_start)
// - if frame_count <= 0: decode until stream end from frame_start
//
// Memory ownership:
// - On success, *out_data is allocated with malloc() and must be released by
//   calling avenc_free_float32().
//
// Returns 0 on success, non-zero on error.
int avenc_decode_subset_to_float32(
    const char *input_path,
    int stream_id,
    int ni_start, int nj_start, int width, int height,
    int frame_start, int frame_count,
    float **out_data,
    int *out_width, int *out_height,
    int *out_frame_count);

void avenc_free_float32(float *ptr);

// Thread-local last error string for the current thread.
const char *avenc_decode_subset_last_error(void);

// Thread-local ffmpeg command used by the last decode call.
const char *avenc_decode_subset_last_ffmpeg_command(void);

#ifdef __cplusplus
}
#endif

