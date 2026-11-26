/* SPDX-License-Identifier: MIT */

#include "snapshot.h"
#include "internal.h"
#include "kmpp_vcodec.h"
#include "rga_scale.h"
#include "rk_mpp_encoder.h"
#include "v4l2_mplane.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t snapshot_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t snapshot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t snapshot_cv;
static pthread_once_t snapshot_once = PTHREAD_ONCE_INIT;
static struct {
    void *source;
    int fd;
    int width;
    int height;
    size_t bytes;
    uint64_t pts;
    bool pending;
    bool cancelled;
} snapshot_job = {.fd = -1};

static void snapshot_init(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&snapshot_cv, &attr);
    pthread_condattr_destroy(&attr);
}

static uint64_t snapshot_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static int snapshot_remaining(uint64_t deadline)
{
    uint64_t now = snapshot_now();
    return now >= deadline ? 0 : (int)((deadline - now) / 1000000ull);
}

static int snapshot_ioctl(int fd, uint32_t cmd, uint32_t ctrl, void *data, uint32_t size)
{
    struct kmpp_vcodec_msg msg = {
        .cmd = cmd, .ctrl_cmd = ctrl, .size = size, .data_ptr = (uint64_t)(uintptr_t)data,
    };
    return ioctl(fd, KMPP_VOCDEC_IOC_CFG, &msg);
}

void rk_snapshot_offer(void *source, int fd, int width, int height, uint32_t fourcc,
                        uint32_t stride, size_t bytes, uint64_t pts_ns)
{
    pthread_once(&snapshot_once, snapshot_init);
    pthread_mutex_lock(&snapshot_lock);
    if (snapshot_job.source == source && snapshot_job.pending && !snapshot_job.cancelled) {
        if (width != snapshot_job.width || height != snapshot_job.height ||
            fourcc != V4L2_PIX_FMT_NV16 || stride != (uint32_t)width || fd < 0) {
            snapshot_job.cancelled = true;
        } else {
            snapshot_job.fd = fd;
            snapshot_job.bytes = bytes;
            snapshot_job.pts = pts_ns;
        }
        snapshot_job.pending = false;
        pthread_cond_broadcast(&snapshot_cv);
    }
    pthread_mutex_unlock(&snapshot_lock);
}

bool rk_snapshot_holds(void *source, int fd)
{
    bool held;
    pthread_mutex_lock(&snapshot_lock);
    held = snapshot_job.source == source && snapshot_job.fd >= 0 && snapshot_job.fd == fd;
    pthread_mutex_unlock(&snapshot_lock);
    return held;
}

bool rk_snapshot_source_busy(void *source)
{
    bool busy;
    pthread_mutex_lock(&snapshot_lock);
    busy = snapshot_job.source == source && snapshot_job.fd >= 0;
    pthread_mutex_unlock(&snapshot_lock);
    return busy;
}

void rk_snapshot_quiesce(void *source)
{
    pthread_once(&snapshot_once, snapshot_init);
    pthread_mutex_lock(&snapshot_lock);
    if (snapshot_job.source == source) {
        snapshot_job.cancelled = true;
        snapshot_job.pending = false;
        pthread_cond_broadcast(&snapshot_cv);
        while (snapshot_job.fd >= 0) {
            pthread_cond_wait(&snapshot_cv, &snapshot_lock);
        }
    }
    pthread_mutex_unlock(&snapshot_lock);
}

static void snapshot_release_input(void)
{
    pthread_mutex_lock(&snapshot_lock);
    snapshot_job.fd = -1;
    pthread_cond_broadcast(&snapshot_cv);
    pthread_mutex_unlock(&snapshot_lock);
}

static int snapshot_jpeg_open(int width, int height, double quality)
{
    struct kmpp_vcodec_attr attr = {
        .type = KMPP_CTX_ENC, .coding = KMPP_VIDEO_CODING_MJPEG, .chan_id = 1,
        .buf_size = (uint32_t)width * (uint32_t)((height + 15) & ~15) * 2u,
        .max_strm_cnt = 1, .max_width = (uint32_t)width, .max_height = (uint32_t)height,
    };
    struct kmpp_onekvm_setup setup = {
        .width = (uint32_t)width, .height = (uint32_t)height,
        .hor_stride = (uint32_t)width, .ver_stride = (uint32_t)height,
        .fmt = KMPP_FMT_YUV420SP, .fps = 1, .gop = 1,
        .coding = KMPP_VIDEO_CODING_MJPEG,
    };
    struct kmpp_jpeg_quality codec = {
        .coding = KMPP_VIDEO_CODING_MJPEG, .change = 1u << 2,
        .q_factor = quality == 0 ? 80 : (int32_t)(quality * 99),
    };
    int fd = open("/dev/vcodec", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (codec.q_factor < 1) {
        codec.q_factor = 1;
    }
    codec.qf_min = codec.qf_max = codec.q_factor;
    /* JPEG carries no VUI: the colour-free setup works on every kernel. */
    if (snapshot_ioctl(fd, KMPP_VCODEC_CHAN_CREATE, 0, &attr, sizeof(attr)) != 0 ||
        snapshot_ioctl(fd, KMPP_VCODEC_CHAN_ONEKVM_SETUP, 0, &setup,
                       (uint32_t)KMPP_ONEKVM_SETUP_V1_SIZE) != 0 ||
        snapshot_ioctl(fd, KMPP_VCODEC_CHAN_CONTROL, KMPP_ENC_SET_CODEC_CFG, &codec, sizeof(codec)) != 0 ||
        snapshot_ioctl(fd, KMPP_VCODEC_CHAN_START, 0, NULL, 0) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int32_t rk_source_snapshot(void *source,
    const struct onekvm_video_snapshot_request_v1 *request,
    uint8_t *data, uint64_t capacity, struct onekvm_video_snapshot_result_v1 *result,
    char *error, uint32_t error_capacity)
{
    uint64_t deadline;
    struct timespec until;
    struct rk_rga_dma *rga = NULL;
    struct kmpp_dma_alloc dma = {.fd = -1};
    struct kmpp_venc_packet packet = {0};
    struct kmpp_frame_infos frame = {0};
    void *temporary = NULL;
    int jpeg = -1;
    int status = ONEKVM_VIDEO_RESOURCE_BUSY;
    int width;
    int height;
    int dw;
    int dh;
    int input_fd;
    bool signal;
    size_t bytes;

    if (source == NULL || request == NULL || result == NULL ||
        request->struct_size < sizeof(*request) || result->struct_size < sizeof(*result) ||
        request->flags != 0 || request->timeout_ms > 1000 ||
        !isfinite(request->quality_factor) || request->quality_factor < 0 || request->quality_factor > 1 ||
        request->width < 0 || request->height < 0 ||
        (request->width == 0) != (request->height == 0) || (data == NULL && capacity != 0)) {
        onekvm_set_error(error, error_capacity, "invalid snapshot request");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    result->data_size = 0;
    result->width = result->height = 0;
    result->pts_ns = 0;
    deadline = snapshot_now() + (request->timeout_ms == 0 ? 250ull : request->timeout_ms) * 1000000ull;
    if (pthread_mutex_trylock(&snapshot_gate) != 0) {
        onekvm_set_error(error, error_capacity, "snapshot already in progress");
        return ONEKVM_VIDEO_RESOURCE_BUSY;
    }
    pthread_once(&snapshot_once, snapshot_init);
    rk_source_timings(source, &width, &height, NULL, &signal);
    dw = request->width == 0 ? width : request->width;
    dh = request->height == 0 ? height : request->height;
    if (request->width == 0 && rk_source_output_size(source, width, height, &dw, &dh) < 0) {
        signal = false;
    }
    if (!signal || !onekvm_scale_supported(width, height, dw, dh)) {
        status = ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
        onekvm_set_error(error, error_capacity, "snapshot requires signal and aligned downscale up to 8:1");
        goto done;
    }
    rga = rk_rga_dma_open(V4L2_PIX_FMT_NV16, width, height, (uint32_t)width, dw, dh, &bytes);
    if (rga == NULL || snapshot_remaining(deadline) == 0) {
        goto unavailable;
    }
    jpeg = snapshot_jpeg_open(dw, dh, request->quality_factor);
    dma.size = (uint32_t)onekvm_nv12_dma_bytes(dw, dh, bytes);
    if (jpeg < 0 || snapshot_ioctl(jpeg, KMPP_VCODEC_CHAN_ONEKVM_ALLOC_DMA, 0, &dma, sizeof(dma)) != 0 || dma.fd < 0) {
        goto unavailable;
    }
    pthread_mutex_lock(&snapshot_lock);
    snapshot_job.source = source;
    snapshot_job.width = width;
    snapshot_job.height = height;
    snapshot_job.fd = -1;
    snapshot_job.cancelled = false;
    snapshot_job.pending = true;
    pthread_mutex_unlock(&snapshot_lock);
    if (!rk_source_capture_active(source)) {
        struct onekvm_video_encoder_config_v1 config = {
            .struct_size = sizeof(config), .codec = ONEKVM_VIDEO_CODEC_H264,
            .fps = 30, .quality_factor = 0.5,
        };
        if (rk_encoder_create(&config, &temporary, error, error_capacity) != 0 ||
            rk_encoder_bind_source(temporary, source, error, error_capacity) != 0) {
            goto unavailable;
        }
    }
    until.tv_sec = (time_t)(deadline / 1000000000ull);
    until.tv_nsec = (long)(deadline % 1000000000ull);
    pthread_mutex_lock(&snapshot_lock);
    while (snapshot_job.pending && !snapshot_job.cancelled) {
        if (pthread_cond_timedwait(&snapshot_cv, &snapshot_lock, &until) == ETIMEDOUT) {
            break;
        }
    }
    input_fd = snapshot_job.fd;
    bytes = snapshot_job.bytes;
    result->pts_ns = snapshot_job.pts;
    if (snapshot_job.cancelled) {
        input_fd = -1;
    }
    snapshot_job.pending = false;
    pthread_mutex_unlock(&snapshot_lock);
    if (input_fd < 0 || rk_rga_dma_run(rga, input_fd, bytes, dma.fd, snapshot_remaining(deadline)) != 0) {
        goto unavailable;
    }
    snapshot_release_input();
    frame.width = (uint32_t)dw;
    frame.height = (uint32_t)dh;
    frame.hor_stride = frame.hor_stride_pixel = (uint32_t)dw;
    frame.ver_stride = (uint32_t)dh;
    frame.fmt = KMPP_FMT_YUV420SP;
    frame.fd = (uint32_t)dma.fd;
    frame.pts = result->pts_ns / 1000ull;
    frame.jpeg_chan_id = -1;
    if (snapshot_remaining(deadline) == 0 ||
        snapshot_ioctl(jpeg, KMPP_VCODEC_CHAN_IN_FRM_RDY, 0, &frame, sizeof(frame)) != 0) {
        goto unavailable;
    }
    {
        struct pollfd wait = {.fd = jpeg, .events = POLLIN};
        int remaining = snapshot_remaining(deadline);
        if (remaining == 0 || poll(&wait, 1, remaining) <= 0 ||
            !(wait.revents & POLLIN) || (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            status = ONEKVM_VIDEO_RESOURCE_TIMEOUT;
            goto unavailable;
        }
    }
    packet.u64packet_addr = (uint64_t)(uintptr_t)data;
    packet.buf_size = capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
    if (snapshot_ioctl(jpeg, KMPP_VCODEC_CHAN_OUT_STRM_BUF_RDY, 0, &packet, sizeof(packet)) != 0 || packet.len == 0) {
        goto unavailable;
    }
    result->data_size = packet.len;
    result->width = dw;
    result->height = dh;
    status = packet.len > capacity ? ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL : ONEKVM_VIDEO_RESOURCE_OK;
    if (snapshot_ioctl(jpeg, KMPP_VCODEC_CHAN_OUT_STRM_END, 0, &packet, sizeof(packet)) != 0) {
        status = ONEKVM_VIDEO_RESOURCE_BUSY;
    }
    if (status == ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL) {
        onekvm_set_error(error, error_capacity, "snapshot buffer too small: need %u bytes", packet.len);
    }
    goto done;
unavailable:
    if (snapshot_remaining(deadline) == 0) {
        status = ONEKVM_VIDEO_RESOURCE_TIMEOUT;
    }
    onekvm_set_error(error, error_capacity, "snapshot unavailable or timed out; live encoder unchanged");
done:
    rk_rga_dma_close(rga);
    snapshot_release_input();
    pthread_mutex_lock(&snapshot_lock);
    snapshot_job.pending = false;
    snapshot_job.source = NULL;
    pthread_mutex_unlock(&snapshot_lock);
    if (jpeg >= 0) {
        (void)snapshot_ioctl(jpeg, KMPP_VCODEC_CHAN_STOP, 0, NULL, 0);
        close(jpeg);
    }
    if (dma.fd >= 0) {
        close(dma.fd);
    }
    if (temporary != NULL) {
        rk_encoder_destroy(temporary);
    }
    pthread_mutex_unlock(&snapshot_gate);
    return status;
}
