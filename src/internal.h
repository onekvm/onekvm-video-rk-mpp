#ifndef ONEKVM_VIDEO_RK_MPP_INTERNAL_H
#define ONEKVM_VIDEO_RK_MPP_INTERNAL_H

#include <onekvm/video_backend_v1.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ONEKVM_VIDEO_INTERNAL __attribute__((visibility("hidden")))

#define ONEKVM_RK_MPP_DRIVER_ID "rk-mpp"
#define ONEKVM_RK_MPP_DEFAULT_DEVICE "/dev/video0"
/* JetKVM uses 3 CIF slots and QBUF as soon as SendFrame returns. kmpp
 * reads the DMABUF in place, so we hold one slot until the AU. 4 = 2 in
 * CIF + 1 in VEPU + 1 spare. CIF needs 2 queued to catch every SOF at
 * 60 Hz; 1 slot drops to 30. Capture_ns is FS timestamp → DQ, not
 * QBUF residency (that is ~2 frames even with a shallow queue). */
#define ONEKVM_RK_MPP_BUFFER_COUNT 4

static inline void onekvm_set_error(char *error, uint32_t capacity, const char *fmt, ...)
{
    if (error == NULL || capacity == 0) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, capacity, fmt, args);
    va_end(args);
    error[capacity - 1] = '\0';
}

static inline const char *onekvm_device_path(const struct onekvm_video_source_config_v1 *config)
{
    if (config == NULL || config->device == NULL || config->device[0] == '\0') {
        return ONEKVM_RK_MPP_DEFAULT_DEVICE;
    }
    return config->device;
}

static inline bool onekvm_map_resolution(int32_t resolution, int32_t *width, int32_t *height)
{
    if (width == NULL || height == NULL) {
        return false;
    }
    switch (resolution) {
    case 1080:
        *width = 1920;
        *height = 1080;
        return true;
    case 720:
        *width = 1280;
        *height = 720;
        return true;
    case 480:
        *width = 640;
        *height = 480;
        return true;
    default:
        if (resolution >= 10000) {
            *width = resolution / 10000;
            *height = resolution % 10000;
            return *width >= 48 && *width <= 1920 && (*width % 16) == 0 &&
                   *height >= 34 && *height <= 1080 && (*height % 2) == 0;
        }
        return false;
    }
}

static inline bool onekvm_scale_supported(int width, int height, int dw, int dh)
{
    return dw >= 48 && dh >= 34 && dw <= width && dh <= height &&
           dw <= 1920 && dh <= 1080 && dw % 16 == 0 && dh % 2 == 0 &&
           width <= dw * 8 && height <= dh * 8;
}

/* RGA writes exactly sizeimage, but VEPU and the JPEG encoder fetch whole
 * coding-block rows. Pad the NV12 allocation to 64 rows so the last partial
 * block row stays inside the DMABUF. */
static inline size_t onekvm_nv12_dma_bytes(int width, int height, size_t sizeimage)
{
    size_t padded = (size_t)width * (size_t)((height + 63) & ~63) * 3u / 2u;
    return padded > sizeimage ? padded : sizeimage;
}

static inline double onekvm_clamp_quality(double quality)
{
    if (quality <= 0.0) {
        return 0.5;
    }
    if (quality > 1.0) {
        return 1.0;
    }
    return quality;
}

static inline int onekvm_bitrate_kbps(int width, int height, double quality)
{
    int bitrate;

    if (width <= 0 || height <= 0) {
        width = 1920;
        height = 1080;
    }
    bitrate = (int)((double)(width * height) / (1920.0 * 1080.0) * 4000.0 *
                     onekvm_clamp_quality(quality));
    if (bitrate < 200) {
        bitrate = 200;
    }
    return bitrate;
}

/* JetKVM-style VBR target. Core default quality 0.5 is 2 Mbps CBR at 1080p60.
 * onekvm-server remb will not be changed; 8 Mbps + qp_max 40 overflowed the
 * WebRTC pipe (~3 Mbps) and the drops looked like motion smear. High+CABAC
 * at 3 Mbps stays inside remb; 5 Mbps VBR bursts to ~7.5 Mbps and the
 * 4-deep WebRTC send queue drops mid-GOP (Chrome overlay 0 FPS). */
static inline int onekvm_target_bitrate_kbps(int width, int height, double quality,
                                             int32_t override_kbps)
{
    int bitrate;

    if (override_kbps > 0) {
        bitrate = override_kbps;
    } else {
        bitrate = onekvm_bitrate_kbps(width, height, quality);
        if (height >= 1080 && bitrate < 3000) {
            bitrate = 3000;
        } else if (height >= 720 && bitrate < 2000) {
            bitrate = 2000;
        }
    }
    if (bitrate < 200) {
        bitrate = 200;
    }
    if (bitrate > 4000) {
        bitrate = 4000;
    }
    return bitrate;
}

/* Match JetKVM: encode at HDMI refresh, cap 60. 4 CIF DMABUFs, 2 kept queued. */
static inline int onekvm_encode_fps(int src_fps, int height)
{
    int fps = src_fps > 0 ? src_fps : 30;

    (void)height;
    if (fps > 60) {
        return 60;
    }
    return fps;
}

static inline int32_t onekvm_auto_gop(int32_t gop, int32_t fps)
{
    /* JetKVM: GOP = fps/2. Honor an explicit core override other than
     * the server default of 60. */
    if (gop > 0 && gop != 60) {
        return gop;
    }
    if (fps > 1) {
        return fps / 2;
    }
    return 30;
}

static inline uint32_t onekvm_resolved_codec(uint32_t codec)
{
    if (codec == ONEKVM_VIDEO_CODEC_AUTO) {
        return ONEKVM_VIDEO_CODEC_H264;
    }
    return codec;
}

static inline bool onekvm_codec_supported(uint32_t codec)
{
    return codec == ONEKVM_VIDEO_CODEC_AUTO || codec == ONEKVM_VIDEO_CODEC_H264 ||
           codec == ONEKVM_VIDEO_CODEC_H265;
}

/* Last stage duration. 0 = not sampled yet. Reject >=1s (stall / wrap). */
static inline void onekvm_store_stage_ns(uint64_t *slot, uint64_t ns)
{
    if (slot != NULL && ns > 0 && ns < 1000000000ull) {
        __atomic_store_n(slot, ns, __ATOMIC_RELAXED);
    }
}

static inline uint64_t onekvm_load_stage_ns(uint64_t *slot)
{
    return slot == NULL ? 0 : __atomic_load_n(slot, __ATOMIC_RELAXED);
}

static inline int32_t onekvm_fill_latency(struct onekvm_video_latency_v1 *latency,
                                          uint64_t capture_ns, uint64_t encode_ns)
{
    if (latency == NULL || latency->struct_size < sizeof(*latency)) {
        return -1;
    }
    latency->capture_ns = capture_ns;
    latency->encode_ns = encode_ns;
    return 0;
}

#endif
