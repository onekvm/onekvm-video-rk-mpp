#ifndef ONEKVM_VIDEO_RK_MPP_V4L2_H
#define ONEKVM_VIDEO_RK_MPP_V4L2_H

#include <onekvm/video_backend_v1.h>

#include <stdbool.h>

int32_t rk_source_create(const struct onekvm_video_source_config_v1 *config, void **source,
                          char *error, uint32_t error_capacity);
int32_t rk_source_reset(void *source, const struct onekvm_video_source_config_v1 *config,
                        char *error, uint32_t error_capacity);
int32_t rk_source_read(void *source, struct onekvm_video_frame_v1 *frame, char *error,
                       uint32_t error_capacity);
void rk_source_release(void *source, uint64_t token);
int32_t rk_source_signal_present(void *source);
void rk_source_interrupt(void *source);
void rk_source_destroy(void *source);
int32_t rk_source_input_format(void *source, struct onekvm_video_format_v1 *format);
int32_t rk_source_latency(void *source, struct onekvm_video_latency_v1 *latency);
void rk_source_note_capture_ns(void *source, uint64_t capture_ns);
void rk_source_publish_display(void *source, const uint8_t *src, int src_fd, uint32_t fourcc,
                               int32_t width, int32_t height, uint32_t stride, uint64_t pts_ns);
void rk_source_release_display(void *source);
int32_t rk_source_display(void *source, const struct onekvm_video_display_request_v1 *request,
                          uint8_t *data, uint64_t capacity,
                          struct onekvm_video_display_result_v1 *result, char *error,
                          uint32_t error_capacity);

int rk_source_subdev_fd(void *source);
int rk_source_apply_subdev_format(void *source, int32_t width, int32_t height);
/* The bridge's YCbCr output as V4L2_YCBCR_ENC_* and V4L2_QUANTIZATION_*;
 * -1 when it is not YCbCr or not reported. */
int rk_source_ycbcr(void *source, uint32_t *encoding, uint32_t *quantization);
int rk_source_lock_detected_timings(void *source);
void rk_source_timings(void *source, int32_t *width, int32_t *height, int32_t *fps, bool *signal);
const char *rk_source_device(void *source);
int rk_source_output_size(void *source, int width, int height, int *dw, int *dh);
bool rk_source_capture_active(void *source);
bool rk_source_capture_claim(void *source, void *encoder);
void rk_source_capture_release(void *source, void *encoder);

int32_t rk_edid_capabilities(struct onekvm_video_edid_caps_v1 *caps);
int32_t rk_edid_get(struct onekvm_video_edid_blob_v1 *edid, char *error, uint32_t error_capacity);
int32_t rk_edid_set(const struct onekvm_video_edid_blob_v1 *edid,
                    struct onekvm_video_edid_apply_result_v1 *apply, char *error,
                    uint32_t error_capacity);
int32_t rk_edid_set_mode(const struct onekvm_video_edid_blob_v1 *edid, uint32_t mode,
                         struct onekvm_video_edid_apply_result_v1 *apply, char *error,
                         uint32_t error_capacity);

#endif
