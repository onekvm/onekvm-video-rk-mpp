/* SPDX-License-Identifier: MIT */

/* CPU display fallback: each bridge's YCbCr maps black, white and the
 * primaries back to the RGB the source sent. */

#include "display_csc.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>

struct rgb {
    int r;
    int g;
    int b;
};

static struct rgb unpack(uint16_t px)
{
    struct rgb c = {(px >> 11) << 3, ((px >> 5) & 0x3f) << 2, (px & 0x1f) << 3};
    return c;
}

/* Encode R'G'B' (0..255) the way the bridge does and decode it again. */
static struct rgb round_trip(uint32_t encoding, uint32_t quantization, double r, double g, double b)
{
    double kr = encoding == V4L2_YCBCR_ENC_709 ? 0.2126 : 0.299;
    double kb = encoding == V4L2_YCBCR_ENC_709 ? 0.0722 : 0.114;
    double y = kr * r + (1.0 - kr - kb) * g + kb * b;
    double cb = (b - y) / (2.0 * (1.0 - kb));
    double cr = (r - y) / (2.0 * (1.0 - kr));
    int yy;
    int u;
    int v;

    if (quantization == V4L2_QUANTIZATION_FULL_RANGE) {
        yy = (int)lround(y);
        u = (int)lround(128.0 + cb);
        v = (int)lround(128.0 + cr);
    } else {
        yy = (int)lround(16.0 + y * 219.0 / 255.0);
        u = (int)lround(128.0 + cb * 224.0 / 255.0);
        v = (int)lround(128.0 + cr * 224.0 / 255.0);
    }
    return unpack(rk_yuv_rgb565(rk_yuv_coef_for(encoding, quantization), yy, u, v));
}

static void expect(struct rgb got, int r, int g, int b)
{
    /* RGB565 keeps 5/6/5 bits; allow one step plus rounding. */
    assert(abs(got.r - (r & 0xf8)) <= 8);
    assert(abs(got.g - (g & 0xfc)) <= 4);
    assert(abs(got.b - (b & 0xf8)) <= 8);
}

int main(void)
{
    static const uint32_t encodings[] = {V4L2_YCBCR_ENC_601, V4L2_YCBCR_ENC_709};
    static const uint32_t ranges[] = {V4L2_QUANTIZATION_LIM_RANGE, V4L2_QUANTIZATION_FULL_RANGE};
    static const int colors[][3] = {
        {0, 0, 0}, {255, 255, 255}, {128, 128, 128}, {255, 0, 0},
        {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {32, 64, 200},
    };
    unsigned e;
    unsigned q;
    unsigned c;

    for (e = 0; e < 2; e++) {
        for (q = 0; q < 2; q++) {
            for (c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
                expect(round_trip(encodings[e], ranges[q], colors[c][0], colors[c][1], colors[c][2]),
                       colors[c][0], colors[c][1], colors[c][2]);
            }
        }
    }
    /* Full-range black and white must not clip as limited range would. */
    assert(rk_yuv_rgb565(rk_yuv_coef_for(V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_FULL_RANGE), 8, 128,
                         128) != 0);
    assert(rk_yuv_rgb565(rk_yuv_coef_for(V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_FULL_RANGE), 245, 128,
                         128) != 0xffff);
    /* Unknown matrix and range keep BT.601 limited range. */
    assert(rk_yuv_coef_for(V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT) ==
           rk_yuv_coef_for(V4L2_YCBCR_ENC_601, V4L2_QUANTIZATION_LIM_RANGE));
    return 0;
}
