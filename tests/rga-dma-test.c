#include <assert.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <string.h>

static unsigned queued;
static unsigned dequeued;
static unsigned stopped;
static int timeout;
static int corrupt;

static int mock_open(const char *path, int flags, ...)
{
    assert(strcmp(path, "/dev/mock-rga") == 0);
    assert(flags & O_NONBLOCK);
    return 42;
}

static int mock_close(int fd)
{
    assert(fd == 42);
    return 0;
}

static int mock_poll(struct pollfd *fds, nfds_t count, int milliseconds)
{
    assert(count == 1 && fds[0].fd == 42 && milliseconds == 25);
    fds[0].revents = timeout ? 0 : POLLIN;
    return timeout ? 0 : 1;
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
        unsigned width = fmt->fmt.pix_mp.width;
        unsigned height = fmt->fmt.pix_mp.height;
        fmt->fmt.pix_mp.plane_fmt[0].bytesperline = width;
        fmt->fmt.pix_mp.plane_fmt[0].sizeimage = width * height *
            (fmt->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 2 : 3) /
            (fmt->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 1 : 2);
        if (corrupt) {
            fmt->fmt.pix_mp.width++;
        }
    } else if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *buf = argument;
        assert(buf->memory == V4L2_MEMORY_DMABUF);
        assert(buf->m.planes[0].m.fd ==
               (buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 10 : 11));
        queued++;
    } else if (request == VIDIOC_DQBUF) {
        dequeued++;
    } else if (request == VIDIOC_STREAMOFF) {
        stopped++;
    }
    return 0;
}

#define open mock_open
#define close mock_close
#define poll mock_poll
#define ioctl mock_ioctl
#include "../src/rga_scale.c"
#undef open
#undef close
#undef poll
#undef ioctl

int main(void)
{
    size_t bytes;
    struct rk_rga_dma *rga;
    g_rga.scanned = true;
    g_rga.present = true;
    strcpy(g_rga.path, "/dev/mock-rga");
    rga = rk_rga_dma_open(V4L2_PIX_FMT_NV16, 1920, 1080, 1920, 640, 480, &bytes);
    assert(rga != NULL && bytes == 460800);
    assert(rk_rga_dma_run(rga, 10, 1920 * 1088 * 2, 11, 25) == 0);
    assert(queued == 2 && dequeued == 2);
    timeout = 1;
    assert(rk_rga_dma_run(rga, 10, 1920 * 1088 * 2, 11, 25) != 0);
    rk_rga_dma_close(rga);
    assert(stopped == 2);
    corrupt = 1;
    assert(rk_rga_dma_open(V4L2_PIX_FMT_NV16, 1920, 1080, 1920, 640, 480, &bytes) == NULL);
    assert(rk_rga_dma_open(V4L2_PIX_FMT_NV16, 1920, 1080, 2048, 640, 480, &bytes) == NULL);
    return 0;
}
