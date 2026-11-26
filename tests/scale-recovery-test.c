/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "kmpp_vcodec.h"

static int fail_alloc;
static uint32_t alloc_size;
static int reject_setup_size;
static uint32_t setup_size;
static struct kmpp_onekvm_setup setup_seen;

static int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    struct kmpp_vcodec_msg *msg;

    assert(fd == 42 && request == KMPP_VOCDEC_IOC_CFG);
    va_start(arguments, request);
    msg = va_arg(arguments, struct kmpp_vcodec_msg *);
    va_end(arguments);
    if (msg->cmd == KMPP_VCODEC_CHAN_ONEKVM_ALLOC_DMA) {
        struct kmpp_dma_alloc *alloc = (void *)(uintptr_t)msg->data_ptr;
        if (fail_alloc) {
            return -1;
        }
        alloc_size = alloc->size;
        alloc->fd = eventfd(0, EFD_CLOEXEC);
    } else if (msg->cmd == KMPP_VCODEC_CHAN_ONEKVM_SETUP) {
        if (msg->size == (uint32_t)reject_setup_size || reject_setup_size < 0) {
            return -1;
        }
        setup_size = msg->size;
        memset(&setup_seen, 0, sizeof(setup_seen));
        memcpy(&setup_seen, (void *)(uintptr_t)msg->data_ptr, msg->size);
    }
    return 0;
}

#define ioctl mock_ioctl
#include "../src/rk_mpp_encoder.c"
#undef ioctl

static int fail_open;
static unsigned rga_opens;
static unsigned rga_closes;
static atomic_uint device_queries;
static int rga_token;
static int source_token;

struct rk_rga_dma *rk_rga_dma_open(uint32_t fourcc, int width, int height, uint32_t stride,
                                   int dw, int dh, size_t *bytes)
{
    assert(fourcc == V4L2_PIX_FMT_NV16 && width == 1920 && height == 1080 && stride == 1920);
    assert(dw == 640 && dh == 480);
    rga_opens++;
    if (fail_open) {
        return NULL;
    }
    *bytes = (size_t)dw * (size_t)dh * 3u / 2u;
    return (struct rk_rga_dma *)&rga_token;
}

int rk_rga_dma_run(struct rk_rga_dma *rga, int src_fd, size_t src_bytes, int dst_fd,
                   int timeout_ms)
{
    (void)rga;
    (void)src_fd;
    (void)src_bytes;
    (void)dst_fd;
    (void)timeout_ms;
    return -1;
}

void rk_rga_dma_close(struct rk_rga_dma *rga)
{
    if (rga != NULL) {
        assert(rga == (struct rk_rga_dma *)&rga_token);
        rga_closes++;
    }
}

bool rk_rga_present(void)
{
    return true;
}

/* The bound capture only needs a source that never opens a device: it then
 * shows the no-signal frame until unbind. */
const char *rk_source_device(void *source)
{
    assert(source == &source_token);
    device_queries++;
    return "/nonexistent/rk-mpp-test-video";
}

void rk_source_timings(void *source, int32_t *width, int32_t *height, int32_t *fps, bool *signal)
{
    (void)source;
    if (width != NULL) {
        *width = 1920;
    }
    if (height != NULL) {
        *height = 1080;
    }
    if (fps != NULL) {
        *fps = 60;
    }
    if (signal != NULL) {
        *signal = true;
    }
}

int rk_source_lock_detected_timings(void *source)
{
    (void)source;
    return 0;
}

int rk_source_apply_subdev_format(void *source, int32_t width, int32_t height)
{
    (void)source;
    (void)width;
    (void)height;
    return 0;
}

static int ycbcr_result = -1;
static uint32_t ycbcr_encoding;
static uint32_t ycbcr_quantization;

int rk_source_ycbcr(void *source, uint32_t *encoding, uint32_t *quantization)
{
    (void)source;
    *encoding = ycbcr_encoding;
    *quantization = ycbcr_quantization;
    return ycbcr_result;
}

int rk_source_subdev_fd(void *source)
{
    (void)source;
    return -1;
}

void rk_source_note_capture_ns(void *source, uint64_t capture_ns)
{
    (void)source;
    (void)capture_ns;
}

void rk_source_publish_display(void *source, const uint8_t *src, int src_fd, uint32_t fourcc,
                               int32_t width, int32_t height, uint32_t stride, uint64_t pts_ns)
{
    (void)source;
    (void)src;
    (void)src_fd;
    (void)fourcc;
    (void)width;
    (void)height;
    (void)stride;
    (void)pts_ns;
}

void rk_source_release_display(void *source)
{
    (void)source;
}

int rk_source_output_size(void *source, int width, int height, int *dw, int *dh)
{
    (void)source;
    *dw = 640;
    *dh = 480;
    return width == 1920 && height == 1080 ? 1 : -1;
}

bool rk_source_capture_claim(void *source, void *encoder)
{
    (void)source;
    (void)encoder;
    return true;
}

void rk_source_capture_release(void *source, void *encoder)
{
    (void)source;
    (void)encoder;
}

void rk_snapshot_offer(void *source, int fd, int width, int height, uint32_t fourcc,
                       uint32_t stride, size_t bytes, uint64_t pts_ns)
{
    (void)source;
    (void)fd;
    (void)width;
    (void)height;
    (void)fourcc;
    (void)stride;
    (void)bytes;
    (void)pts_ns;
}

bool rk_snapshot_holds(void *source, int fd)
{
    (void)source;
    (void)fd;
    return false;
}

bool rk_snapshot_source_busy(void *source)
{
    (void)source;
    return false;
}

void rk_snapshot_quiesce(void *source)
{
    (void)source;
}

static struct rk_encoder *make_encoder(void)
{
    struct onekvm_video_encoder_config_v1 config = {
        .struct_size = sizeof(config), .codec = ONEKVM_VIDEO_CODEC_H264,
    };
    void *opaque;
    char error[128];

    assert(rk_encoder_create(&config, &opaque, error, sizeof(error)) == 0);
    return opaque;
}

static uint32_t setup_color(struct rk_encoder *enc, int expect)
{
    struct kmpp_onekvm_setup setup;

    memset(&setup, 0, sizeof(setup));
    assert(kmpp_setup_channel(enc, 42, &setup) == expect);
    return atomic_load(&enc->stream_range);
}

static void test_setup_color(struct rk_encoder *enc)
{
    /* Nothing reported: nothing signalled. */
    assert(setup_color(enc, 0) == ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED);
    assert(setup_size == sizeof(struct kmpp_onekvm_setup));
    assert(setup_seen.range == 0 && setup_seen.matrix == 0);

    /* TC358743 UYVY: BT.601 limited, primaries and transfer left alone. */
    ycbcr_result = 0;
    ycbcr_encoding = V4L2_YCBCR_ENC_601;
    ycbcr_quantization = V4L2_QUANTIZATION_LIM_RANGE;
    assert(setup_color(enc, 0) == ONEKVM_VIDEO_COLOR_RANGE_LIMITED);
    assert(setup_seen.range == KMPP_RANGE_MPEG && setup_seen.matrix == KMPP_SPC_SMPTE170M);
    assert(setup_seen.primaries == 0 && setup_seen.transfer == 0);

    /* Limited range without a matrix writes no video_signal_type. */
    ycbcr_encoding = V4L2_YCBCR_ENC_BT2020;
    assert(setup_color(enc, 0) == ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED);
    assert(setup_seen.range == KMPP_RANGE_MPEG && setup_seen.matrix == 0);

    /* Full range is signalled on its own. */
    ycbcr_encoding = V4L2_YCBCR_ENC_709;
    ycbcr_quantization = V4L2_QUANTIZATION_FULL_RANGE;
    assert(setup_color(enc, 0) == ONEKVM_VIDEO_COLOR_RANGE_FULL);
    assert(setup_seen.range == KMPP_RANGE_JPEG && setup_seen.matrix == KMPP_SPC_BT709);

    /* A kernel without the colour fields takes the short struct. */
    ycbcr_encoding = V4L2_YCBCR_ENC_601;
    ycbcr_quantization = V4L2_QUANTIZATION_LIM_RANGE;
    reject_setup_size = (int)sizeof(struct kmpp_onekvm_setup);
    assert(setup_color(enc, 0) == ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED);
    assert(setup_size == KMPP_ONEKVM_SETUP_V1_SIZE && setup_seen.width == 0);
    reject_setup_size = -1;
    assert(setup_color(enc, -1) == ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED);
    reject_setup_size = 0;
    ycbcr_result = -1;
}

int main(void)
{
    struct rk_encoder *enc = make_encoder();
    char error[128];

    test_setup_color(enc);

    /* kmpp_open_encoder records the scaled channel size. */
    enc->active_width = 640;
    enc->active_height = 480;
    /* Scaled setup: RGA context plus a padded NV12 buffer. */
    assert(kmpp_open_scale(enc, 42, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, 1, 640, 480));
    assert(enc->scaled && enc->scale != NULL && enc->scale_fd >= 0);
    assert(alloc_size == 640u * 512u * 3u / 2u);

    /* Transient failures reopen the RGA context in place. */
    scale_recover(enc, V4L2_PIX_FMT_NV16, 1920, 1080, 1920);
    assert(enc->scale != NULL && !enc->scale_disabled && enc->scale_failures == 1);
    scale_recover(enc, V4L2_PIX_FMT_NV16, 1920, 1080, 1920);
    assert(enc->scale != NULL && !enc->scale_disabled && enc->scale_failures == 2);
    /* The third failure in a row gives up on scaling for this binding. */
    scale_recover(enc, V4L2_PIX_FMT_NV16, 1920, 1080, 1920);
    assert(enc->scale == NULL && enc->scale_disabled && enc->scaled);
    assert(rga_opens == 3 && rga_closes == 3);
    kmpp_close_encoder(enc);
    assert(!enc->scaled && enc->scale_fd < 0);

    /* Once disabled, a reopen keeps the native size without touching RGA. */
    assert(!kmpp_open_scale(enc, 42, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, 1, 640, 480));
    assert(!enc->scaled && enc->scale == NULL && rga_opens == 3);
    enc->scale_disabled = false;

    /* A failed reopen disables scaling at once. */
    assert(kmpp_open_scale(enc, 42, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, 1, 640, 480));
    fail_open = 1;
    scale_recover(enc, V4L2_PIX_FMT_NV16, 1920, 1080, 1920);
    assert(enc->scale == NULL && enc->scale_disabled && enc->scale_failures == 1);
    fail_open = 0;
    kmpp_close_encoder(enc);
    enc->scale_disabled = false;

    /* Native fallback: infeasible size, no RGA, no CMA. */
    assert(!kmpp_open_scale(enc, 42, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, -1, 640, 480));
    assert(!enc->scaled && rga_opens == 5);
    fail_open = 1;
    assert(!kmpp_open_scale(enc, 42, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, 1, 640, 480));
    assert(!enc->scaled && enc->scale == NULL);
    fail_open = 0;
    fail_alloc = 1;
    assert(!kmpp_open_scale(enc, 42, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, 1, 640, 480));
    assert(!enc->scaled && enc->scale == NULL && enc->scale_fd < 0);
    assert(rga_opens == 7 && rga_closes == 5);
    fail_alloc = 0;

    /* Bind: a live capture thread is reused; one that returned is restarted
     * instead of leaving a dead binding behind. */
    assert(rk_encoder_bind_source(enc, &source_token, error, sizeof(error)) == 0);
    usleep(20000);
    assert(device_queries == 1);
    assert(rk_encoder_bind_source(enc, &source_token, error, sizeof(error)) == 0);
    assert(device_queries == 1);
    atomic_store(&enc->capture_exited, true);
    assert(rk_encoder_bind_source(enc, &source_token, error, sizeof(error)) == 0);
    usleep(20000);
    assert(device_queries == 2 && !atomic_load(&enc->capture_exited));
    assert(rk_encoder_unbind_source(enc, error, sizeof(error)) == 0);
    assert(!enc->capture_started && !atomic_load(&enc->capture_exited));
    rk_encoder_destroy(enc);
    return 0;
}
