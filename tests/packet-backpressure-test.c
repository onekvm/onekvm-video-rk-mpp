#include <assert.h>
#include <stdarg.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <string.h>
#include "kmpp_vcodec.h"

static unsigned gets;
static unsigned ends;
static unsigned waits;
static unsigned qbufs;
static int held_index = -1;

static int mock_poll(struct pollfd *fds, nfds_t count, int milliseconds)
{
    if (count == 2) {
        fds[0].revents = POLLIN;
        fds[1].revents = 0;
        return 1;
    }
    if (milliseconds == 16) {
        waits++;
        usleep(16000);
    } else {
        assert(milliseconds == 0);
    }
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
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *buf = argument;
        assert((int)buf->index != held_index);
        qbufs++;
    } else {
        struct kmpp_vcodec_msg *msg = argument;
        struct kmpp_venc_packet *packet = (void *)(uintptr_t)msg->data_ptr;
        assert(request == KMPP_VOCDEC_IOC_CFG);
        if (msg->cmd == KMPP_VCODEC_CHAN_OUT_STRM_BUF_RDY) {
            uint8_t *data = (void *)(uintptr_t)packet->u64packet_addr;
            memset(data, 0x55, 64);
            data[0] = data[1] = data[2] = 0;
            data[3] = 1;
            data[4] = 0x65;
            packet->len = 64;
            packet->buf_size = 64;
            gets++;
        } else {
            assert(msg->cmd == KMPP_VCODEC_CHAN_OUT_STRM_END);
            ends++;
        }
    }
    return 0;
}

#define poll mock_poll
#define ioctl mock_ioctl
#include "../src/rk_mpp_encoder.c"
#include "../src/snapshot.c"
#undef poll
#undef ioctl

int main(void)
{
    pthread_once(&snapshot_once, snapshot_init);
    struct onekvm_video_encoder_config_v1 config = {
        .struct_size = sizeof(config), .codec = ONEKVM_VIDEO_CODEC_H264,
    };
    void *opaque;
    struct rk_encoder *encoder;
    char error[128];
    unsigned slot;
    uint64_t before;
    assert(rk_encoder_create(&config, &opaque, error, sizeof(error)) == 0);
    encoder = opaque;
    encoder->vcodec_fd = 42;
    encoder->stream_cap = 64;
    encoder->in_flight = 1;
    for (slot = 0; slot < RK_AU_SLABS; slot++) {
        encoder->au_slab[slot] = malloc(64);
        assert(encoder->au_slab[slot] != NULL);
        encoder->au_slab_busy[slot] = 1;
    }
    before = monotonic_ns();
    assert(kmpp_drain_packets(encoder, 0) == 0);
    assert(monotonic_ns() - before < 100000000ull);
    assert(waits == 1 && gets == 0 && ends == 0 && encoder->in_flight == 1);
    pthread_mutex_lock(&encoder->mutex);
    encoder->slab_wait = true;
    au_slab_release_locked(encoder, 2);
    assert(!encoder->slab_wait);
    pthread_mutex_unlock(&encoder->mutex);
    {
        struct pollfd woke = {.fd = encoder->drain_wake, .events = POLLIN};
        assert(poll(&woke, 1, 0) == 1);
        clear_wake_fd(encoder->drain_wake);
    }
    assert(kmpp_drain_packets(encoder, 0) == 1);
    assert(gets == 1 && ends == 1 && encoder->in_flight == 0 && encoder->au_done == 1);
    assert(encoder->q_count == 1 && encoder->queue[0].slab == 2);
    encoder->vcodec_fd = -1;
    rk_encoder_destroy(encoder);
    {
        struct rk_cif_hold hold = {0};
        int dma_fds[4] = {100, 101, 102, 103};
        int source_token;
        hold.source = &source_token;
        hold.spare[0] = 0;
        hold.spare[1] = 1;
        hold.spare_n = 2;
        snapshot_job.source = &source_token;
        snapshot_job.fd = 100;
        held_index = 0;
        cif_feed(42, V4L2_MEMORY_DMABUF, dma_fds, 4096, &hold);
        assert(qbufs == 1 && hold.cif_n == 1 && hold.spare_n == 1);
        assert(hold.spare[0] == 0);
        snapshot_release_input();
        held_index = -1;
        cif_feed(42, V4L2_MEMORY_DMABUF, dma_fds, 4096, &hold);
        assert(qbufs == 2 && hold.cif_n == 2 && hold.spare_n == 0);
        snapshot_job.source = NULL;
    }
    return 0;
}
