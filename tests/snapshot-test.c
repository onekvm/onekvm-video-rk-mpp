/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include "kmpp_vcodec.h"
#include "rga_scale.h"
#include "rk_mpp_encoder.h"
#include "v4l2_mplane.h"

static int live = 1;
static int signal_present = 1;
static int fail_rga;
static int fail_poll;
static int fail_setup;
static unsigned creates;
static unsigned returns;
static unsigned stops;
static unsigned temporary_created;
static unsigned temporary_destroyed;
static unsigned rga_opened;
static unsigned rga_closed;
static unsigned jpeg_opened;
static unsigned jpeg_closed;
static unsigned dma_allocated;
static unsigned dma_closed;
static int requested_width;
static int requested_height;
static pthread_t producer;
static int source_token;

static int mock_open(const char *filename, int flags, ...)
{
    assert(strcmp(filename, "/dev/vcodec") == 0);
    assert(flags & O_NONBLOCK);
    jpeg_opened++;
    return 42;
}

static int mock_close(int fd)
{
    assert(fd == 42 || fd == 101);
    if (fd == 42) {
        jpeg_closed++;
    } else {
        dma_closed++;
    }
    return 0;
}

static int mock_poll(struct pollfd *fds, nfds_t count, int milliseconds)
{
    assert(count == 1 && fds[0].fd == 42 && milliseconds > 0 && milliseconds <= 250);
    fds[0].revents = fail_poll ? 0 : POLLIN;
    return fail_poll ? 0 : 1;
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    struct kmpp_vcodec_msg *msg;
    assert(fd == 42 && request == KMPP_VOCDEC_IOC_CFG);
    va_start(arguments, request);
    msg = va_arg(arguments, struct kmpp_vcodec_msg *);
    va_end(arguments);
    if (msg->cmd == KMPP_VCODEC_CHAN_CREATE) {
        struct kmpp_vcodec_attr *attr = (void *)(uintptr_t)msg->data_ptr;
        assert(attr->coding == 8 && attr->chan_id == 1 && attr->max_strm_cnt == 1);
        creates++;
    } else if (msg->cmd == KMPP_VCODEC_CHAN_ONEKVM_SETUP) {
        struct kmpp_onekvm_setup *setup = (void *)(uintptr_t)msg->data_ptr;
        assert(setup->coding == 8 && setup->fmt == 0);
        assert(setup->width == (unsigned)requested_width && setup->height == (unsigned)requested_height);
        return fail_setup ? -1 : 0;
    } else if (msg->cmd == KMPP_VCODEC_CHAN_CONTROL) {
        struct kmpp_jpeg_quality *quality = (void *)(uintptr_t)msg->data_ptr;
        assert(msg->size == 2832);
        assert(msg->ctrl_cmd == 0x00320007);
        assert(quality->change == 4 && quality->q_factor >= 1 && quality->q_factor <= 99);
        assert(quality->qf_max == quality->q_factor && quality->qf_min == quality->q_factor);
    } else if (msg->cmd == KMPP_VCODEC_CHAN_ONEKVM_ALLOC_DMA) {
        struct kmpp_dma_alloc *dma = (void *)(uintptr_t)msg->data_ptr;
        assert(dma->size == (unsigned)(requested_width * ((requested_height + 63) & ~63) * 3 / 2));
        dma->fd = 101;
        dma_allocated++;
    } else if (msg->cmd == KMPP_VCODEC_CHAN_IN_FRM_RDY) {
        struct kmpp_frame_infos *frame = (void *)(uintptr_t)msg->data_ptr;
        assert(frame->fd == 101 && frame->jpeg_chan_id == -1 && frame->fmt == 0);
        assert(frame->pts == 123456 && frame->width == (unsigned)requested_width);
    } else if (msg->cmd == KMPP_VCODEC_CHAN_OUT_STRM_BUF_RDY) {
        struct kmpp_venc_packet *packet = (void *)(uintptr_t)msg->data_ptr;
        if (packet->buf_size >= 32) {
            uint8_t *output = (void *)(uintptr_t)packet->u64packet_addr;
            memset(output, 0x55, 32);
            output[0] = 0xff;
            output[1] = 0xd8;
            output[30] = 0xff;
            output[31] = 0xd9;
        }
        packet->len = 32;
    } else if (msg->cmd == KMPP_VCODEC_CHAN_OUT_STRM_END) {
        returns++;
    } else if (msg->cmd == KMPP_VCODEC_CHAN_STOP) {
        stops++;
    }
    return 0;
}

#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#define poll mock_poll
#include "../src/snapshot.c"
#undef open
#undef close
#undef ioctl
#undef poll

void rk_source_timings(void *source, int32_t *width, int32_t *height, int32_t *fps, bool *signal)
{
    assert(source == &source_token && fps == NULL);
    *width = 1920;
    *height = 1080;
    *signal = signal_present;
}

bool rk_source_capture_active(void *source)
{
    assert(source == &source_token);
    return live != 0;
}

int rk_source_output_size(void *source, int width, int height, int *dw, int *dh)
{
    assert(source == &source_token);
    *dw = width;
    *dh = height;
    return 0;
}

static void *produce(void *ignored)
{
    (void)ignored;
    for (;;) {
        bool pending;
        pthread_mutex_lock(&snapshot_lock);
        pending = snapshot_job.pending;
        pthread_mutex_unlock(&snapshot_lock);
        if (pending) {
            break;
        }
        usleep(100);
    }
    rk_snapshot_offer(&source_token, 100, 1920, 1080, V4L2_PIX_FMT_NV16,
                        1920, 1920u * 1088u * 2u, 123456000);
    return NULL;
}

int32_t rk_encoder_create(const struct onekvm_video_encoder_config_v1 *config, void **encoder,
                           char *error, uint32_t error_capacity)
{
    (void)config;
    (void)error;
    (void)error_capacity;
    *encoder = &temporary_created;
    temporary_created++;
    return 0;
}

int32_t rk_encoder_bind_source(void *encoder, void *source, char *error, uint32_t error_capacity)
{
    (void)encoder;
    (void)source;
    (void)error;
    (void)error_capacity;
    assert(pthread_create(&producer, NULL, produce, NULL) == 0);
    return 0;
}

void rk_encoder_destroy(void *encoder)
{
    (void)encoder;
    pthread_join(producer, NULL);
    temporary_destroyed++;
}

struct rk_rga_dma *rk_rga_dma_open(uint32_t fourcc, int width, int height,
                                   uint32_t stride, int dw, int dh, size_t *bytes)
{
    assert(fourcc == V4L2_PIX_FMT_NV16 && width == 1920 && height == 1080 && stride == 1920);
    assert(dw == requested_width && dh == requested_height);
    *bytes = (size_t)dw * dh * 3 / 2;
    rga_opened++;
    return (struct rk_rga_dma *)&rga_closed;
}

int rk_rga_dma_run(struct rk_rga_dma *rga, int src_fd, size_t src_bytes, int dst_fd, int timeout_ms)
{
    (void)rga;
    assert(src_fd == 100 && dst_fd == 101 && src_bytes == 1920u * 1088u * 2u);
    assert(timeout_ms > 0 && timeout_ms <= 250);
    assert(rk_snapshot_holds(&source_token, 100));
    return fail_rga ? -1 : 0;
}

void rk_rga_dma_close(struct rk_rga_dma *rga)
{
    if (rga != NULL) {
        rga_closed++;
    }
}

static int capture(struct onekvm_video_snapshot_request_v1 *request,
                   uint8_t *output, uint64_t capacity, bool offer)
{
    char error[256] = {0};
    struct onekvm_video_snapshot_result_v1 result = {.struct_size = sizeof(result)};
    int status;
    if (live && offer) {
        assert(pthread_create(&producer, NULL, produce, NULL) == 0);
    }
    status = rk_source_snapshot(&source_token, request, output, capacity, &result, error, sizeof(error));
    if (live && offer) {
        pthread_join(producer, NULL);
    }
    assert(!rk_snapshot_holds(&source_token, 100));
    /* Nothing outlives the call: the JPEG channel, its NV12 input, the RGA
     * context and a temporary capture are released on every path, so an
     * idle snapshot keeps no CMA. */
    assert(jpeg_closed == jpeg_opened && dma_closed == dma_allocated);
    assert(rga_closed == rga_opened && temporary_destroyed == temporary_created);
    if (status == 0 || status == ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL) {
        assert(result.data_size == 32 && result.pts_ns == 123456000);
        assert(result.width == requested_width && result.height == requested_height);
    }
    return status;
}

int main(void)
{
    struct onekvm_video_snapshot_request_v1 request = {
        .struct_size = sizeof(request), .width = 640, .height = 480, .timeout_ms = 100,
        .quality_factor = 0.7,
    };
    uint8_t output[40];
    requested_width = 640;
    requested_height = 480;
    assert(capture(&request, output, sizeof(output), true) == 0);
    assert(output[0] == 0xff && output[1] == 0xd8 && output[31] == 0xd9);
    memset(output, 0xaa, sizeof(output));
    assert(capture(&request, output, 2, true) == ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL);
    assert(output[0] == 0xaa && output[2] == 0xaa);
    assert(capture(&request, NULL, 0, true) == ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL);
    fail_rga = 1;
    assert(capture(&request, output, sizeof(output), true) == ONEKVM_VIDEO_RESOURCE_BUSY);
    fail_rga = 0;
    fail_poll = 1;
    assert(capture(&request, output, sizeof(output), true) == ONEKVM_VIDEO_RESOURCE_TIMEOUT);
    fail_poll = 0;
    request.timeout_ms = 15;
    {
        uint64_t before = snapshot_now();
        assert(capture(&request, output, sizeof(output), false) == ONEKVM_VIDEO_RESOURCE_TIMEOUT);
        assert(snapshot_now() - before < 100000000ull);
    }
    request.timeout_ms = 100;
    live = 0;
    assert(capture(&request, output, sizeof(output), true) == 0);
    assert(temporary_created == 1 && temporary_destroyed == 1);
    live = 1;
    request.width = request.height = 0;
    requested_width = 1920;
    requested_height = 1080;
    assert(capture(&request, output, sizeof(output), true) == 0);
    fail_setup = 1;
    assert(capture(&request, output, sizeof(output), false) == ONEKVM_VIDEO_RESOURCE_BUSY);
    fail_setup = 0;
    signal_present = 0;
    assert(capture(&request, output, sizeof(output), false) == ONEKVM_VIDEO_RESOURCE_UNSUPPORTED);
    signal_present = 1;
    request.width = 641;
    request.height = 480;
    assert(capture(&request, output, sizeof(output), false) == ONEKVM_VIDEO_RESOURCE_UNSUPPORTED);
    request.flags = 1;
    assert(capture(&request, output, sizeof(output), false) == ONEKVM_VIDEO_RESOURCE_INVALID);
    request.flags = 0;
    request.quality_factor = NAN;
    assert(capture(&request, output, sizeof(output), false) == ONEKVM_VIDEO_RESOURCE_INVALID);
    assert(returns == 5 && creates == jpeg_closed && stops + 1 == creates);
    assert(dma_allocated == 8 && rga_opened == 9);
    return 0;
}
