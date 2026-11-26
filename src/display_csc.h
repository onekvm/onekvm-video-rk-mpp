/* SPDX-License-Identifier: MIT */
#ifndef ONEKVM_VIDEO_RK_MPP_DISPLAY_CSC_H
#define ONEKVM_VIDEO_RK_MPP_DISPLAY_CSC_H

#include <linux/videodev2.h>

#include <stdint.h>

/* YCbCr → RGB for the CPU display fallback, 8.8 fixed point. */
struct rk_yuv_coef {
    int y_offset;
    int y;
    int rv;
    int gu;
    int gv;
    int bu;
};

/* Unknown encoding or range keeps BT.601 limited range. */
static inline const struct rk_yuv_coef *rk_yuv_coef_for(uint32_t encoding, uint32_t quantization)
{
    static const struct rk_yuv_coef bt601_limited = {16, 298, 409, 100, 208, 516};
    static const struct rk_yuv_coef bt709_limited = {16, 298, 459, 55, 136, 541};
    static const struct rk_yuv_coef bt601_full = {0, 256, 359, 88, 183, 454};
    static const struct rk_yuv_coef bt709_full = {0, 256, 403, 48, 120, 475};
    int bt709 = encoding == V4L2_YCBCR_ENC_709;

    if (quantization == V4L2_QUANTIZATION_FULL_RANGE) {
        return bt709 ? &bt709_full : &bt601_full;
    }
    return bt709 ? &bt709_limited : &bt601_limited;
}

static inline int rk_clamp_u8(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

static inline uint16_t rk_yuv_rgb565(const struct rk_yuv_coef *k, int y, int u, int v)
{
    int c = (y - k->y_offset) * k->y;
    int d = u - 128;
    int e = v - 128;
    int r = rk_clamp_u8((c + k->rv * e + 128) >> 8);
    int g = rk_clamp_u8((c - k->gu * d - k->gv * e + 128) >> 8);
    int b = rk_clamp_u8((c + k->bu * d + 128) >> 8);

    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

#endif
