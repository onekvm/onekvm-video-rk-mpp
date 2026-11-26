/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "kmpp_vcodec.h"

static uint32_t next_flag;
static uint8_t next_nal = 0x41;

static int mock_poll(struct pollfd *fds, nfds_t count, int milliseconds)
{
    (void)milliseconds;
    if (count == 2) {
        fds[0].revents = POLLIN;
        fds[1].revents = 0;
        return 1;
    }
    return 0;
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    struct kmpp_vcodec_msg *msg;
    struct kmpp_venc_packet *packet;

    assert(fd == 42 && request == KMPP_VOCDEC_IOC_CFG);
    va_start(arguments, request);
    msg = va_arg(arguments, struct kmpp_vcodec_msg *);
    va_end(arguments);
    packet = (void *)(uintptr_t)msg->data_ptr;
    if (msg->cmd == KMPP_VCODEC_CHAN_OUT_STRM_BUF_RDY) {
        uint8_t *data = (void *)(uintptr_t)packet->u64packet_addr;
        memset(data, 0x55, 64);
        data[0] = data[1] = data[2] = 0;
        data[3] = 1;
        data[4] = next_nal;
        packet->len = 64;
        packet->flag = next_flag;
    } else {
        assert(msg->cmd == KMPP_VCODEC_CHAN_OUT_STRM_END);
    }
    return 0;
}

#define poll mock_poll
#define ioctl mock_ioctl
#include "../src/rk_mpp_encoder.c"
#undef poll
#undef ioctl

static struct rk_encoder *make_encoder(void)
{
    struct onekvm_video_encoder_config_v1 config = {
        .struct_size = sizeof(config), .codec = ONEKVM_VIDEO_CODEC_H264,
    };
    void *opaque;
    char error[128];
    unsigned slot;
    struct rk_encoder *encoder;

    assert(rk_encoder_create(&config, &opaque, error, sizeof(error)) == 0);
    encoder = opaque;
    encoder->stream_cap = 64;
    for (slot = 0; slot < RK_AU_SLABS; slot++) {
        encoder->au_slab[slot] = malloc(64);
        assert(encoder->au_slab[slot] != NULL);
    }
    return encoder;
}

/* Heap AUs bypass the slab limit, so the queue itself can overflow. */
static void commit(struct rk_encoder *encoder, uint8_t tag, int key)
{
    uint8_t *data = malloc(1);
    assert(data != NULL);
    data[0] = tag;
    pthread_mutex_lock(&encoder->mutex);
    queue_commit_locked(encoder, data, 1, RK_AU_HEAP, key, tag);
    pthread_mutex_unlock(&encoder->mutex);
}

static int read_tag(struct rk_encoder *encoder)
{
    struct onekvm_video_packet_v1 packet = {.struct_size = sizeof(packet)};
    char error[64];

    assert(rk_encoder_read_packet(encoder, &packet, error, sizeof(error)) == 0);
    rk_encoder_release_packet(encoder);
    return packet.data_size == 0 ? -1 : (int)packet.pts_ns;
}

static void color(struct rk_encoder *encoder, uint32_t bit_depth, uint32_t range)
{
    struct onekvm_video_color_info_v1 info = {.struct_size = sizeof(info)};

    assert(rk_encoder_color_info(encoder, &info) == ONEKVM_VIDEO_RESOURCE_OK);
    assert(info.bit_depth == bit_depth && info.color_range == range);
}

static void test_color_info(void)
{
    struct rk_encoder *encoder = make_encoder();
    struct onekvm_video_color_info_v1 info = {.struct_size = sizeof(info) - 1};

    assert(rk_encoder_color_info(encoder, &info) == ONEKVM_VIDEO_RESOURCE_INVALID);
    assert(rk_encoder_color_info(encoder, NULL) == ONEKVM_VIDEO_RESOURCE_INVALID);
    info.struct_size = sizeof(info);
    assert(rk_encoder_color_info(NULL, &info) == ONEKVM_VIDEO_RESOURCE_INVALID);

    /* Unknown until a keyframe is delivered, not merely queued. */
    color(encoder, 0, ONEKVM_VIDEO_COLOR_RANGE_UNKNOWN);
    commit(encoder, 1, 1);
    color(encoder, 0, ONEKVM_VIDEO_COLOR_RANGE_UNKNOWN);
    assert(read_tag(encoder) == 1);
    color(encoder, 8, ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED);

    /* Each AU keeps the range its channel signalled when it was queued, so a
     * reopened channel (no-signal, then capture) does not relabel AUs still
     * queued. P-frames leave the reported value alone. */
    atomic_store(&encoder->stream_range, ONEKVM_VIDEO_COLOR_RANGE_LIMITED);
    commit(encoder, 2, 1);
    atomic_store(&encoder->stream_range, ONEKVM_VIDEO_COLOR_RANGE_FULL);
    commit(encoder, 3, 1);
    commit(encoder, 4, 0);
    assert(read_tag(encoder) == 2);
    color(encoder, 8, ONEKVM_VIDEO_COLOR_RANGE_LIMITED);
    assert(read_tag(encoder) == 3);
    color(encoder, 8, ONEKVM_VIDEO_COLOR_RANGE_FULL);
    atomic_store(&encoder->stream_range, ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED);
    assert(read_tag(encoder) == 4);
    color(encoder, 8, ONEKVM_VIDEO_COLOR_RANGE_FULL);
    rk_encoder_destroy(encoder);
}

int main(void)
{
    struct rk_encoder *encoder;

    test_color_info();
    encoder = make_encoder();

    /* Overflow without a later keyframe: the whole GOP goes, the incoming
     * P-frame too, and an IDR is forced. */
    commit(encoder, 1, 1);
    commit(encoder, 2, 0);
    commit(encoder, 3, 0);
    commit(encoder, 4, 0);
    commit(encoder, 5, 0);
    assert(encoder->q_count == 0 && encoder->qdrop == 5);
    assert(encoder->await_key && encoder->force_keyframe);
    commit(encoder, 6, 0);
    assert(encoder->q_count == 0 && encoder->qdrop == 6);
    commit(encoder, 7, 1);
    commit(encoder, 8, 0);
    assert(!encoder->await_key && encoder->q_count == 2);
    assert(read_tag(encoder) == 7 && read_tag(encoder) == 8 && read_tag(encoder) == -1);

    /* A queued keyframe ends the drop; nothing is forced. */
    encoder->force_keyframe = false;
    commit(encoder, 10, 1);
    commit(encoder, 11, 0);
    commit(encoder, 12, 1);
    commit(encoder, 13, 0);
    commit(encoder, 14, 0);
    assert(encoder->qdrop == 8 && !encoder->await_key && !encoder->force_keyframe);
    assert(read_tag(encoder) == 12 && read_tag(encoder) == 13 && read_tag(encoder) == 14);

    /* An incoming keyframe needs no forced IDR even if the GOP is gone. */
    commit(encoder, 20, 1);
    commit(encoder, 21, 0);
    commit(encoder, 22, 0);
    commit(encoder, 23, 0);
    commit(encoder, 24, 1);
    assert(encoder->qdrop == 12 && !encoder->await_key && !encoder->force_keyframe);
    assert(read_tag(encoder) == 24 && read_tag(encoder) == -1);

    /* A deliberate flush also blocks stale P-frames. */
    commit(encoder, 30, 0);
    pthread_mutex_lock(&encoder->mutex);
    queue_flush_locked(encoder);
    pthread_mutex_unlock(&encoder->mutex);
    assert(encoder->q_count == 0 && encoder->await_key && encoder->force_keyframe);

    /* The drain still completes the in-flight frame of a discarded AU, so
     * capture returns its CIF slot, and releases the slab. */
    encoder->vcodec_fd = 42;
    encoder->in_flight = 1;
    next_flag = 0;
    next_nal = 0x41;
    assert(kmpp_drain_packets(encoder, 0) == 1);
    assert(encoder->q_count == 0 && encoder->qdrop == 13);
    assert(encoder->in_flight == 0 && encoder->au_done == 1);
    assert(!encoder->au_slab_busy[0] && !encoder->au_slab_busy[1]);
    encoder->in_flight = 1;
    next_flag = KMPP_PACKET_FLAG_INTRA;
    assert(kmpp_drain_packets(encoder, 0) == 1);
    assert(encoder->q_count == 1 && !encoder->await_key && encoder->au_done == 2);
    encoder->in_flight = 1;
    next_flag = 0;
    assert(kmpp_drain_packets(encoder, 0) == 1);
    assert(encoder->q_count == 2 && encoder->qdrop == 13);

    encoder->vcodec_fd = -1;
    rk_encoder_destroy(encoder);
    return 0;
}
