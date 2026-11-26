/* SPDX-License-Identifier: MIT */
#ifndef ONEKVM_VIDEO_RK_MPP_NO_SIGNAL_H
#define ONEKVM_VIDEO_RK_MPP_NO_SIGNAL_H

#include <stddef.h>
#include <stdint.h>

/* Identity of the NV21 file behind current/<WxH>.nv21. Zero when it is absent. */
struct rk_no_signal_token {
    uint64_t device;
    uint64_t inode;
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

void rk_no_signal_token(int width, int height, struct rk_no_signal_token *token);

/* Draw the cached no-signal NV21 nearest to width x height into an NV12
 * frame with the given luma stride and rows before the chroma plane,
 * letterboxed in black. A missing cache gives a black frame. */
int rk_no_signal_render_nv12(uint8_t *dst, size_t capacity, int width, int height,
                             uint32_t stride, uint32_t ver_stride);

#endif
