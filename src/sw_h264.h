#ifndef ONEKVM_VIDEO_RK_MPP_SW_H264_H
#define ONEKVM_VIDEO_RK_MPP_SW_H264_H

#include <stdint.h>

/* Software baseline H.264 (minih264e). PicoKVM 6.18 has no kmpp; A7 cannot
 * encode 1080p, so the bound path scales HDMI into this size. */
#define RK_SW_H264_WIDTH 640
#define RK_SW_H264_HEIGHT 352

struct rk_sw_h264;

int rk_sw_h264_open(struct rk_sw_h264 **out);
void rk_sw_h264_close(struct rk_sw_h264 *enc);

void rk_sw_h264_fill_bars(struct rk_sw_h264 *enc);
void rk_sw_h264_load_yuyv(struct rk_sw_h264 *enc, const uint8_t *src, int src_w, int src_h,
                          int stride);
void rk_sw_h264_load_uyvy(struct rk_sw_h264 *enc, const uint8_t *src, int src_w, int src_h,
                          int stride);
void rk_sw_h264_load_nv16(struct rk_sw_h264 *enc, const uint8_t *src, int src_w, int src_h,
                          int y_stride);

/* Output is annex-B and valid until the next encode/close. */
int rk_sw_h264_encode(struct rk_sw_h264 *enc, int keyframe, uint8_t **out, int *out_size);

#endif
