/* SPDX-License-Identifier: MIT */

/* The display scaler must not keep a capture buffer alive once capture
 * stops. The mock models vb2: a DMABUF queued on OUTPUT stays attached
 * after DQBUF and is dropped only by REQBUFS(0) or a different DMABUF.
 * The OUTPUT format must carry the source's YCbCr matrix and range. */

#include <assert.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static int attached = -1;
static bool output_streaming;
static unsigned setups;
static unsigned mapped;
static uint8_t frame[240 * 240 * 2];
static struct v4l2_pix_format_mplane out_fmt;

static int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(strcmp(path, "/dev/mock-rga") == 0);
    return 42;
}

static int mock_close(int fd)
{
    assert(fd == 42);
    return 0;
}

static int mock_poll(struct pollfd *fds, nfds_t count, int milliseconds)
{
    (void)milliseconds;
    assert(count == 1 && fds[0].fd == 42);
    fds[0].revents = POLLIN;
    return 1;
}

static void *mock_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
    (void)address;
    (void)protection;
    (void)flags;
    (void)offset;
    assert(fd == 42 && length == sizeof(frame));
    mapped++;
    return frame;
}

static int mock_munmap(void *address, size_t length)
{
    assert(address == frame && length == sizeof(frame) && mapped > 0);
    mapped--;
    return 0;
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    void *argument;
    assert(fd == 42);
    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *fmt = argument;
        if (fmt->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            out_fmt = fmt->fmt.pix_mp;
        }
    } else if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *req = argument;
        if (req->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            assert(!output_streaming && req->memory == V4L2_MEMORY_DMABUF);
            if (req->count == 0) {
                attached = -1;
            } else {
                setups++;
            }
        }
    } else if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *buf = argument;
        buf->m.planes[0].length = sizeof(frame);
        buf->m.planes[0].m.mem_offset = 0;
    } else if (request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF) {
        if (*(enum v4l2_buf_type *)argument == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            output_streaming = request == VIDIOC_STREAMON;
        }
    } else if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *buf = argument;
        if (buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            assert(output_streaming && buf->memory == V4L2_MEMORY_DMABUF);
            attached = buf->m.planes[0].m.fd;
        }
    }
    return 0;
}

#define open mock_open
#define close mock_close
#define poll mock_poll
#define mmap mock_mmap
#define munmap mock_munmap
#define ioctl mock_ioctl
#include "../src/rga_scale.c"
#undef open
#undef close
#undef poll
#undef mmap
#undef munmap
#undef ioctl

int main(void)
{
    uint16_t dst[240 * 240];

    g_rga.scanned = true;
    g_rga.present = true;
    strcpy(g_rga.path, "/dev/mock-rga");
    /* Nothing to release before the first frame. */
    rk_rga_scale_release();
    /* TC358743: BT.601 limited range, also at 1080p. */
    assert(rk_rga_scale_rgb565(7, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, V4L2_YCBCR_ENC_601,
                               V4L2_QUANTIZATION_LIM_RANGE, dst, 240, 240) == 0);
    assert(out_fmt.colorspace == V4L2_COLORSPACE_SMPTE170M &&
           out_fmt.ycbcr_enc == V4L2_YCBCR_ENC_601 &&
           out_fmt.quantization == V4L2_QUANTIZATION_LIM_RANGE);
    assert(rk_rga_scale_rgb565(8, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, V4L2_YCBCR_ENC_601,
                               V4L2_QUANTIZATION_LIM_RANGE, dst, 240, 240) == 0);
    assert(attached == 8 && setups == 1 && mapped == 1);
    /* RK628F: full-range BT.709 sets the session up again. */
    assert(rk_rga_scale_rgb565(8, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, V4L2_YCBCR_ENC_709,
                               V4L2_QUANTIZATION_FULL_RANGE, dst, 240, 240) == 0);
    assert(setups == 2 && out_fmt.colorspace == V4L2_COLORSPACE_REC709 &&
           out_fmt.ycbcr_enc == V4L2_YCBCR_ENC_709 &&
           out_fmt.quantization == V4L2_QUANTIZATION_FULL_RANGE);
    /* Capture stops: the last frame must not stay attached. */
    rk_rga_scale_release();
    assert(attached == -1 && !output_streaming && mapped == 0);
    rk_rga_scale_release();
    /* The next capture sets the session up again. */
    /* Unknown matrix: guess by height, as before. */
    assert(rk_rga_scale_rgb565(9, V4L2_PIX_FMT_NV16, 1920, 1080, 1920, V4L2_YCBCR_ENC_DEFAULT,
                               V4L2_QUANTIZATION_DEFAULT, dst, 240, 240) == 0);
    assert(attached == 9 && setups == 3 && mapped == 1 && !g_rga.dead);
    assert(out_fmt.colorspace == V4L2_COLORSPACE_REC709 &&
           out_fmt.ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT &&
           out_fmt.quantization == V4L2_QUANTIZATION_DEFAULT);
    return 0;
}
