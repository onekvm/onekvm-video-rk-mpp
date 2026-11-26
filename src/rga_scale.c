/* SPDX-License-Identifier: MIT */

#include "rga_scale.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

struct rk_rga {
    char path[64];
    int fd;
    bool scanned;
    bool present;
    atomic_bool dead;
    bool streaming;
    uint32_t src_fourcc;
    int32_t src_w;
    int32_t src_h;
    uint32_t src_encoding;
    uint32_t src_quantization;
    int32_t dst_w;
    int32_t dst_h;
    void *dst_map;
    size_t dst_len;
};

static struct rk_rga g_rga = {
    .fd = -1,
};
static pthread_mutex_t rga_probe_lock = PTHREAD_MUTEX_INITIALIZER;

struct rk_rga_dma {
    int fd;
    size_t bytes;
};

void rk_rga_dma_close(struct rk_rga_dma *rga)
{
    if (rga != NULL) {
        enum v4l2_buf_type output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        enum v4l2_buf_type capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        (void)xioctl(rga->fd, VIDIOC_STREAMOFF, &output);
        (void)xioctl(rga->fd, VIDIOC_STREAMOFF, &capture);
        close(rga->fd);
        free(rga);
    }
}

static bool is_rga_dev(int fd)
{
    struct v4l2_capability cap;
    uint32_t caps;

    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        return false;
    }
    caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) == 0 || (caps & V4L2_CAP_STREAMING) == 0) {
        return false;
    }
    return strstr((const char *)cap.driver, "rga") != NULL ||
           strstr((const char *)cap.card, "rga") != NULL ||
           strstr((const char *)cap.card, "RGA") != NULL;
}

static void scan_rga(void)
{
    DIR *dir;
    struct dirent *ent;

    if (g_rga.scanned) {
        return;
    }
    g_rga.scanned = true;
    dir = opendir("/dev");
    if (dir == NULL) {
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char path[64];
        int fd;

        if (strncmp(ent->d_name, "video", 5) != 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "/dev/%s", ent->d_name) >= (int)sizeof(path)) {
            continue;
        }
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (is_rga_dev(fd)) {
            memcpy(g_rga.path, path, sizeof(g_rga.path));
            g_rga.present = true;
            close(fd);
            fprintf(stderr, "rk-mpp: rga device %s\n", g_rga.path);
            break;
        }
        close(fd);
    }
    closedir(dir);
}

bool rk_rga_present(void)
{
    bool present;
    pthread_mutex_lock(&rga_probe_lock);
    scan_rga();
    present = g_rga.present && !g_rga.dead;
    pthread_mutex_unlock(&rga_probe_lock);
    return present;
}

struct rk_rga_dma *rk_rga_dma_open(uint32_t fourcc, int width, int height,
                                   uint32_t stride, int dw, int dh, size_t *bytes)
{
    struct rk_rga_dma *rga;
    unsigned queue;

    if (bytes == NULL || !rk_rga_present() ||
        (fourcc != V4L2_PIX_FMT_NV16 && fourcc != V4L2_PIX_FMT_NV12) ||
        stride != (uint32_t)width) {
        errno = EINVAL;
        return NULL;
    }
    rga = calloc(1, sizeof(*rga));
    if (rga == NULL) {
        return NULL;
    }
    rga->fd = open(g_rga.path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (rga->fd < 0) {
        free(rga);
        return NULL;
    }
    for (queue = 0; queue < 2; queue++) {
        struct v4l2_format fmt = {0};
        struct v4l2_requestbuffers req = {0};
        enum v4l2_buf_type type = queue == 0 ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
                                                       V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.type = type;
        fmt.fmt.pix_mp.width = queue == 0 ? (uint32_t)width : (uint32_t)dw;
        fmt.fmt.pix_mp.height = queue == 0 ? (uint32_t)height : (uint32_t)dh;
        fmt.fmt.pix_mp.pixelformat = queue == 0 ? fourcc : V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes = 1;
        fmt.fmt.pix_mp.colorspace = height > 576 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
        if (xioctl(rga->fd, VIDIOC_S_FMT, &fmt) < 0 ||
            fmt.fmt.pix_mp.width != (uint32_t)(queue == 0 ? width : dw) ||
            fmt.fmt.pix_mp.height != (uint32_t)(queue == 0 ? height : dh) ||
            fmt.fmt.pix_mp.pixelformat != (queue == 0 ? fourcc : V4L2_PIX_FMT_NV12) ||
            fmt.fmt.pix_mp.num_planes != 1 ||
            fmt.fmt.pix_mp.plane_fmt[0].bytesperline != (uint32_t)(queue == 0 ? width : dw)) {
            goto fail;
        }
        if (queue != 0) {
            rga->bytes = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
            if (rga->bytes < (size_t)dw * dh * 3u / 2u) {
                goto fail;
            }
        }
        req.type = type;
        req.memory = V4L2_MEMORY_DMABUF;
        req.count = 1;
        if (xioctl(rga->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1 ||
            xioctl(rga->fd, VIDIOC_STREAMON, &type) < 0) {
            goto fail;
        }
    }
    *bytes = rga->bytes;
    return rga;
fail:
    rk_rga_dma_close(rga);
    return NULL;
}

int rk_rga_dma_run(struct rk_rga_dma *rga, int src_fd, size_t src_bytes,
                    int dst_fd, int timeout_ms)
{
    unsigned queue;
    struct pollfd wait = {.fd = rga != NULL ? rga->fd : -1, .events = POLLIN};
    if (rga == NULL || src_fd < 0 || dst_fd < 0 || src_bytes > UINT32_MAX || timeout_ms <= 0) {
        return -1;
    }
    for (queue = 0; queue < 2; queue++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buf = {0};
        buf.type = queue == 0 ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.length = 1;
        buf.m.planes = &plane;
        plane.m.fd = queue == 0 ? src_fd : dst_fd;
        plane.length = queue == 0 ? (uint32_t)src_bytes : (uint32_t)rga->bytes;
        plane.bytesused = queue == 0 ? plane.length : 0;
        if (xioctl(rga->fd, VIDIOC_QBUF, &buf) < 0) {
            return -1;
        }
    }
    if (poll(&wait, 1, timeout_ms) <= 0 || !(wait.revents & POLLIN) ||
        (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        return -1;
    }
    for (queue = 0; queue < 2; queue++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buf = {0};
        buf.type = queue == 0 ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.length = 1;
        buf.m.planes = &plane;
        if (xioctl(rga->fd, VIDIOC_DQBUF, &buf) < 0 || (buf.flags & V4L2_BUF_FLAG_ERROR)) {
            return -1;
        }
    }
    return 0;
}

static void rga_stop(void)
{
    enum v4l2_buf_type out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_requestbuffers req;

    if (g_rga.fd < 0) {
        return;
    }
    if (g_rga.streaming) {
        (void)xioctl(g_rga.fd, VIDIOC_STREAMOFF, &out);
        (void)xioctl(g_rga.fd, VIDIOC_STREAMOFF, &cap);
        g_rga.streaming = false;
    }
    if (g_rga.dst_map != NULL && g_rga.dst_map != MAP_FAILED) {
        munmap(g_rga.dst_map, g_rga.dst_len);
    }
    g_rga.dst_map = NULL;
    g_rga.dst_len = 0;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    (void)xioctl(g_rga.fd, VIDIOC_REQBUFS, &req);
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    (void)xioctl(g_rga.fd, VIDIOC_REQBUFS, &req);
}

/* vb2 keeps the last DMABUF queued on OUTPUT attached after DQBUF, so the
 * display session would pin one capture buffer (4 MiB of CMA at 1080p) after
 * capture stops. Drop the queues; the next frame sets them up again. */
void rk_rga_scale_release(void)
{
    rga_stop();
}

static void rga_mark_dead(const char *why)
{
    fprintf(stderr, "rk-mpp: rga disabled: %s (errno=%d)\n", why, errno);
    rga_stop();
    if (g_rga.fd >= 0) {
        close(g_rga.fd);
        g_rga.fd = -1;
    }
    g_rga.dead = true;
}

static int rga_s_fmt(enum v4l2_buf_type type, uint32_t fourcc, int32_t width, int32_t height,
                     uint32_t colorspace, uint32_t encoding, uint32_t quantization)
{
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix_mp.width = (uint32_t)width;
    fmt.fmt.pix_mp.height = (uint32_t)height;
    fmt.fmt.pix_mp.pixelformat = fourcc;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.colorspace = colorspace;
    fmt.fmt.pix_mp.ycbcr_enc = encoding;
    fmt.fmt.pix_mp.quantization = quantization;
    if (xioctl(g_rga.fd, VIDIOC_S_FMT, &fmt) < 0) {
        return -1;
    }
    if (fmt.fmt.pix_mp.pixelformat != fourcc || (int32_t)fmt.fmt.pix_mp.width != width ||
        (int32_t)fmt.fmt.pix_mp.height != height) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* The RGA driver picks its YUV→RGB matrix from these. Without the encoding,
 * guess by height as before. A driver that ignores the range uses limited. */
static uint32_t rga_colorspace(uint32_t encoding, int32_t height)
{
    switch (encoding) {
    case V4L2_YCBCR_ENC_601:
        return V4L2_COLORSPACE_SMPTE170M;
    case V4L2_YCBCR_ENC_709:
        return V4L2_COLORSPACE_REC709;
    case V4L2_YCBCR_ENC_BT2020:
        return V4L2_COLORSPACE_BT2020;
    default:
        return height > 576 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
    }
}

static int rga_ensure(uint32_t fourcc, int32_t width, int32_t height, uint32_t encoding,
                      uint32_t quantization, int32_t dw, int32_t dh)
{
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_plane plane;
    enum v4l2_buf_type out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    uint32_t csc = rga_colorspace(encoding, height);

    if (!rk_rga_present()) {
        return -1;
    }
    if (g_rga.fd < 0) {
        g_rga.fd = open(g_rga.path, O_RDWR | O_CLOEXEC);
        if (g_rga.fd < 0) {
            rga_mark_dead("open");
            return -1;
        }
    }
    if (g_rga.streaming && g_rga.src_fourcc == fourcc && g_rga.src_w == width &&
        g_rga.src_h == height && g_rga.src_encoding == encoding &&
        g_rga.src_quantization == quantization && g_rga.dst_w == dw && g_rga.dst_h == dh &&
        g_rga.dst_map != NULL) {
        return 0;
    }
    rga_stop();
    if (rga_s_fmt(out, fourcc, width, height, csc, encoding, quantization) != 0 ||
        rga_s_fmt(cap, V4L2_PIX_FMT_RGB565, dw, dh, V4L2_COLORSPACE_SRGB,
                  V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT) != 0) {
        rga_mark_dead("S_FMT");
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(g_rga.fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
        rga_mark_dead("REQBUFS output");
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(g_rga.fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
        rga_mark_dead("REQBUFS capture");
        return -1;
    }
    memset(&buf, 0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.m.planes = &plane;
    buf.length = 1;
    if (xioctl(g_rga.fd, VIDIOC_QUERYBUF, &buf) < 0) {
        rga_mark_dead("QUERYBUF");
        return -1;
    }
    g_rga.dst_len = plane.length;
    g_rga.dst_map = mmap(NULL, g_rga.dst_len, PROT_READ, MAP_SHARED, g_rga.fd, plane.m.mem_offset);
    if (g_rga.dst_map == MAP_FAILED) {
        g_rga.dst_map = NULL;
        rga_mark_dead("mmap");
        return -1;
    }
    if (xioctl(g_rga.fd, VIDIOC_STREAMON, &out) < 0 || xioctl(g_rga.fd, VIDIOC_STREAMON, &cap) < 0) {
        rga_mark_dead("STREAMON");
        return -1;
    }
    g_rga.streaming = true;
    g_rga.src_fourcc = fourcc;
    g_rga.src_w = width;
    g_rga.src_h = height;
    g_rga.src_encoding = encoding;
    g_rga.src_quantization = quantization;
    g_rga.dst_w = dw;
    g_rga.dst_h = dh;
    fprintf(stderr, "rk-mpp: rga scale %c%c%c%c %dx%d (enc %u, range %u) -> rgb565 %dx%d\n",
            (int)(fourcc & 0xff), (int)((fourcc >> 8) & 0xff), (int)((fourcc >> 16) & 0xff),
            (int)((fourcc >> 24) & 0xff), width, height, encoding, quantization, dw, dh);
    return 0;
}

int rk_rga_scale_rgb565(int src_fd, uint32_t fourcc, int32_t width, int32_t height, uint32_t stride,
                        uint32_t encoding, uint32_t quantization, uint16_t *dst, int32_t dw,
                        int32_t dh)
{
    struct v4l2_buffer buf;
    struct v4l2_plane plane;
    struct pollfd pfd;
    size_t need;

    (void)stride;
    if (src_fd < 0 || dst == NULL || width < 34 || height < 34 || dw < 34 || dh < 34) {
        return -1;
    }
    if (fourcc != V4L2_PIX_FMT_NV16 && fourcc != V4L2_PIX_FMT_NV12) {
        return -1;
    }
    if (rga_ensure(fourcc, width, height, encoding, quantization, dw, dh) != 0) {
        return -1;
    }
    memset(&buf, 0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = 0;
    buf.m.planes = &plane;
    buf.length = 1;
    plane.m.fd = src_fd;
    plane.bytesused = (uint32_t)((size_t)width * (size_t)height * (fourcc == V4L2_PIX_FMT_NV12 ? 3u : 4u) / 2u);
    plane.length = plane.bytesused;
    if (xioctl(g_rga.fd, VIDIOC_QBUF, &buf) < 0) {
        rga_mark_dead("QBUF output");
        return -1;
    }
    memset(&buf, 0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.m.planes = &plane;
    buf.length = 1;
    if (xioctl(g_rga.fd, VIDIOC_QBUF, &buf) < 0) {
        rga_mark_dead("QBUF capture");
        return -1;
    }
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = g_rga.fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 200) <= 0 || (pfd.revents & POLLIN) == 0) {
        rga_mark_dead("poll");
        return -1;
    }
    memset(&buf, 0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = &plane;
    buf.length = 1;
    if (xioctl(g_rga.fd, VIDIOC_DQBUF, &buf) < 0) {
        rga_mark_dead("DQBUF capture");
        return -1;
    }
    memset(&buf, 0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = &plane;
    buf.length = 1;
    if (xioctl(g_rga.fd, VIDIOC_DQBUF, &buf) < 0) {
        rga_mark_dead("DQBUF output");
        return -1;
    }
    need = (size_t)dw * (size_t)dh * 2u;
    if (g_rga.dst_map == NULL || g_rga.dst_len < need) {
        rga_mark_dead("dst short");
        return -1;
    }
    memcpy(dst, g_rga.dst_map, need);
    return 0;
}
