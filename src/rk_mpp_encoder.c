/* SPDX-License-Identifier: MIT */

#include "rk_mpp_encoder.h"
#include "internal.h"
#include "no_signal.h"
#include "rga_scale.h"
#include "v4l2_mplane.h"
#include "snapshot.h"
#ifndef ONEKVM_HAVE_ROCKIT
#include "kmpp_vcodec.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* No-signal frames: 10 fps on their own channel, an IDR every 2 s, and the
 * cache checked for a new language or custom image once a second. */
#define RK_PLACEHOLDER_NS 100000000ull
#define RK_NS_FPS 10
#define RK_NS_GOP 20
#define RK_NS_CHECK_NS 1000000000ull
#define RK_NS_RETRY_NS 1000000000ull
#define RK_STALL_NS 800000000ull
#define RK_SIGNAL_LOSS_NS 200000000ull
#define RK_RESTART_MIN_NS 2000000000ull
#define RK_STABLE_TIMING_SAMPLES 4
#define RK_MAX_INFLIGHT 1
#define RK_CIF_KEEP 2
#define RK_RELEASE_LAG 0
#define RK_IDR_EVERY 0
#define RK_WAIT_AU_NS 100000000L
#define RK_LIVE_POLL_MS 100
/* Consecutive RGA failures before the binding falls back to native size. */
#define RK_SCALE_MAX_FAILURES 3

#ifdef ONEKVM_HAVE_ROCKIT
#include <rk_common.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_venc.h>
#include <rk_type.h>
#endif

#define RK_PACKET_QUEUE 4
#define RK_PACKET_WAIT_MS 16
/* Every AU is 8-bit 4:2:0. */
#define RK_BIT_DEPTH 8u
#define RK_VEPU_IDLE_NS 250000000ull
#define RK_DRAIN_POLL_MS 200
#define RK_AU_SLABS 4
#define RK_MAX_STREAM_BYTES (1920u * 1080u * 3u / 2u)
#define RK_AU_HEAP (-1)

struct rk_au {
    uint8_t *data;
    size_t size;
    uint32_t key_frame;
    uint64_t pts_ns;
    int slab;
    /* Range the AU's SPS signals: the open channel's when it was queued. */
    uint32_t color_range;
};

struct rk_encoder {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    struct onekvm_video_encoder_config_v1 config;
    void *bound_source;
    pthread_t capture_thread;
    bool capture_started;
    /* Set when capture_drain returns; bind must then start a new thread. */
    atomic_bool capture_exited;
    atomic_bool stop;
    bool force_keyframe;
    /* Queued AUs were dropped: deliver nothing until the next keyframe. */
    bool await_key;
    unsigned qdrop;
    unsigned slab_waits;
#ifdef ONEKVM_HAVE_ROCKIT
    pthread_t venc_thread;
    bool venc_started;
    bool sys_inited;
    int venc_chn;
#endif
    struct rk_au queue[RK_PACKET_QUEUE];
    unsigned q_head;
    unsigned q_count;
    uint8_t *borrowed;
    size_t borrowed_size;
    int borrowed_slab;
    int capture_fd;
    int vcodec_fd;
    int active_width;
    int active_height;
    uint8_t *stream_buf;
    size_t stream_cap;
    uint8_t *au_slab[RK_AU_SLABS];
    uint8_t au_slab_busy[RK_AU_SLABS];
#ifndef ONEKVM_HAVE_ROCKIT
    pthread_t drain_thread;
    bool drain_started;
    atomic_bool drain_stop;
    int capture_wake;
    int drain_wake;
    bool drain_pause;
    bool skip_au;
    bool have_au;
    bool slab_wait;
    struct rk_rga_dma *scale;
    int scale_fd;
    size_t scale_bytes;
    /* VEPU channel expects the RGA NV12 output, not the CIF frame. */
    bool scaled;
    /* RGA failed for this binding; encode the native size until rebind. */
    bool scale_disabled;
    unsigned scale_failures;
    /* The open channel encodes the no-signal frame from the cache, drawn
     * into its own NV12 VEPU buffer, while capture is stopped. */
    bool ns_channel;
    bool ns_drawn;
    int ns_fd;
    uint8_t *ns_map;
    size_t ns_bytes;
    uint32_t ns_stride;
    struct rk_no_signal_token ns_token;
    uint64_t ns_checked_ns;
    uint64_t ns_retry_ns;
    uint64_t ns_submit_ns;
    unsigned au_done;
    unsigned in_flight;
    uint64_t submit_ns[RK_MAX_INFLIGHT];
    unsigned submit_head;
    unsigned submit_n;
#endif
    uint64_t last_capture_ns;
    uint64_t last_encode_ns;
    uint64_t last_read_ns;
    bool vepu_paused;
    /* bit_depth | color_range << 8 of the delivered stream; 0 until a keyframe. */
    atomic_uint color_info;
    /* ONEKVM_VIDEO_COLOR_RANGE_* signalled by the open VEPU channel. */
    atomic_uint stream_range;
};

static pthread_mutex_t g_resource_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_active_h26x;

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

#ifndef ONEKVM_HAVE_ROCKIT
static void wake_fd(int fd)
{
    uint64_t value = 1;
    ssize_t result;

    do {
        result = write(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

static void clear_wake_fd(int fd)
{
    uint64_t value;
    ssize_t result;

    do {
        result = read(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}
#endif

static void au_slab_release_locked(struct rk_encoder *enc, int slab)
{
    if (slab >= 0 && slab < RK_AU_SLABS) {
        enc->au_slab_busy[slab] = 0;
#ifndef ONEKVM_HAVE_ROCKIT
        if (enc->slab_wait) {
            enc->slab_wait = false;
            wake_fd(enc->drain_wake);
        }
#endif
    }
}

static void au_owned_release_locked(struct rk_encoder *enc, uint8_t **data, int *slab)
{
    if (slab != NULL && *slab >= 0) {
        au_slab_release_locked(enc, *slab);
    } else if (slab != NULL && *slab == RK_AU_HEAP && data != NULL) {
        free(*data);
    }
    if (data != NULL) {
        *data = NULL;
    }
    if (slab != NULL) {
        *slab = RK_AU_HEAP;
    }
}

static void queue_clear_locked(struct rk_encoder *enc)
{
    unsigned i;
    for (i = 0; i < RK_PACKET_QUEUE; i++) {
        au_owned_release_locked(enc, &enc->queue[i].data, &enc->queue[i].slab);
        enc->queue[i].size = 0;
    }
    enc->q_head = 0;
    enc->q_count = 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifndef ONEKVM_HAVE_ROCKIT
static void note_capture_ns(struct rk_encoder *enc, uint64_t ns)
{
    onekvm_store_stage_ns(&enc->last_capture_ns, ns);
    rk_source_note_capture_ns(enc->bound_source, ns);
}

static void note_slot_qbuf(uint64_t *qbuf_ns, unsigned index)
{
    if (qbuf_ns != NULL && index < ONEKVM_RK_MPP_BUFFER_COUNT) {
        qbuf_ns[index] = monotonic_ns();
    }
}

static uint64_t v4l2_buf_ts_ns(const struct v4l2_buffer *buf)
{
    if (buf == NULL) {
        return 0;
    }
    return (uint64_t)buf->timestamp.tv_sec * 1000000000ull +
           (uint64_t)buf->timestamp.tv_usec * 1000ull;
}

/* RKCIF CSI stamps FS. That is the fill of this frame, not QBUF wait. */
static void note_cif_capture(struct rk_encoder *enc, const struct v4l2_buffer *buf,
                             uint64_t qbuf_ns)
{
    uint64_t now = monotonic_ns();
    uint64_t ts = v4l2_buf_ts_ns(buf);
    uint64_t ns = 0;

    if (ts != 0 && now > ts && now - ts < 1000000000ull) {
        ns = now - ts;
    } else if (qbuf_ns != 0 && now > qbuf_ns) {
        ns = now - qbuf_ns;
    }
    note_capture_ns(enc, ns);
}

static void latency_submit_reset_locked(struct rk_encoder *enc)
{
    enc->submit_head = 0;
    enc->submit_n = 0;
}

static void latency_submit_push_locked(struct rk_encoder *enc, uint64_t now)
{
    if (enc->submit_n < RK_MAX_INFLIGHT) {
        enc->submit_ns[(enc->submit_head + enc->submit_n) % RK_MAX_INFLIGHT] = now;
        enc->submit_n++;
    }
}

static void latency_submit_pop_tail_locked(struct rk_encoder *enc)
{
    if (enc->submit_n > 0) {
        enc->submit_n--;
    }
}

static void latency_submit_complete_locked(struct rk_encoder *enc, uint64_t now)
{
    uint64_t started;

    if (enc->submit_n == 0) {
        return;
    }
    started = enc->submit_ns[enc->submit_head];
    if (now > started) {
        onekvm_store_stage_ns(&enc->last_encode_ns, now - started);
    }
    enc->submit_head = (enc->submit_head + 1u) % RK_MAX_INFLIGHT;
    enc->submit_n--;
}
#endif

/* `now` is taken before the capture loop locks the encoder. A read that
 * started in between is newer than `now` and means the reader is active;
 * treating it as idle dropped queued AUs and forced an IDR about once a
 * second while streaming. */
static bool reader_active(uint64_t last_read_ns, uint64_t now)
{
    return last_read_ns != 0 &&
           (last_read_ns >= now || now - last_read_ns < RK_VEPU_IDLE_NS);
}

/* Deliberate flush (idle reader, capture restart, signal loss): the caller
 * forces an IDR and no AU older than it may follow. */
static void queue_flush_locked(struct rk_encoder *enc)
{
    queue_clear_locked(enc);
    enc->await_key = true;
    enc->force_keyframe = true;
}

/* Overflow: drop the oldest AU and every queued AU after it up to the next
 * keyframe. Dropping one P-frame alone breaks every later reference until
 * the next IDR (frame_num/POC gaps, grey frames). If no queued keyframe
 * remains and next_key says the incoming AU is not one either, wait for an
 * IDR and force it. */
static void queue_drop_oldest_locked(struct rk_encoder *enc, bool next_key)
{
    if (enc->q_count == 0) {
        return;
    }
    do {
        struct rk_au *slot = &enc->queue[enc->q_head];

        au_owned_release_locked(enc, &slot->data, &slot->slab);
        slot->size = 0;
        enc->q_head = (enc->q_head + 1) % RK_PACKET_QUEUE;
        enc->q_count--;
        enc->qdrop++;
    } while (enc->q_count > 0 && !enc->queue[enc->q_head].key_frame);
    if (enc->q_count == 0 && !next_key) {
        enc->await_key = true;
        enc->force_keyframe = true;
    }
}

static void queue_commit_locked(struct rk_encoder *enc, uint8_t *data, size_t size, int slab,
                                int key_frame, uint64_t pts_ns)
{
    struct rk_au *slot;

    if (enc->q_count == RK_PACKET_QUEUE) {
        queue_drop_oldest_locked(enc, key_frame != 0);
    }
    if (enc->await_key && !key_frame) {
        /* References a dropped AU. Keyframes are never discarded here. */
        au_owned_release_locked(enc, &data, &slab);
        enc->qdrop++;
        return;
    }
    enc->await_key = false;
    slot = &enc->queue[(enc->q_head + enc->q_count) % RK_PACKET_QUEUE];
    slot->data = data;
    slot->size = size;
    slot->slab = slab;
    slot->key_frame = key_frame ? 1u : 0;
    slot->pts_ns = pts_ns;
    slot->color_range = atomic_load(&enc->stream_range);
    enc->q_count++;
    pthread_cond_broadcast(&enc->ready);
}

#ifdef ONEKVM_HAVE_ROCKIT
static void queue_push_locked(struct rk_encoder *enc, const uint8_t *data, size_t size,
                              int key_frame, uint64_t pts_ns)
{
    uint8_t *copy;

    if (data == NULL || size == 0) {
        return;
    }
    copy = malloc(size);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, data, size);
    queue_commit_locked(enc, copy, size, -1, key_frame, pts_ns);
}
#endif

#ifndef ONEKVM_HAVE_ROCKIT
static int au_slab_acquire_locked(struct rk_encoder *enc)
{
    int i;

    for (i = 0; i < RK_AU_SLABS; i++) {
        if (enc->au_slab[i] != NULL && !enc->au_slab_busy[i]) {
            enc->au_slab_busy[i] = 1;
            return i;
        }
    }
    return -1;
}

static void queue_push_slab_locked(struct rk_encoder *enc, int slab, size_t size, int key_frame,
                                   uint64_t pts_ns)
{
    if (slab < 0 || slab >= RK_AU_SLABS || enc->au_slab[slab] == NULL || size == 0) {
        au_slab_release_locked(enc, slab);
        return;
    }
    queue_commit_locked(enc, enc->au_slab[slab], size, slab, key_frame, pts_ns);
}
#endif

#ifndef ONEKVM_HAVE_ROCKIT
static bool au_scan_nal(const uint8_t *data, size_t size, bool *saw_start, bool want_picture)
{
    size_t i;

    *saw_start = false;
    for (i = 0; i + 4 < size; i++) {
        uint8_t type;

        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            type = data[i + 4] & 0x1fu;
            *saw_start = true;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            type = data[i + 3] & 0x1fu;
            *saw_start = true;
        } else {
            continue;
        }
        if (want_picture) {
            if (type == 1 || type == 5) {
                return true;
            }
        } else if (type == 5 || type == 7 || type == 19 || type == 20 || type == 32) {
            return true;
        }
    }
    return false;
}

static bool au_is_keyframe(const uint8_t *data, size_t size)
{
    bool saw_start;

    return au_scan_nal(data, size, &saw_start, false);
}

/* Count coded slices only. SPS/PPS-only packets must not advance au_done or
 * we QBUF a CIF slot VEPU is still reading. No start codes: one AU / frame. */
static bool au_is_picture(const uint8_t *data, size_t size)
{
    bool saw_start;

    if (size < 4) {
        return false;
    }
    if (au_scan_nal(data, size, &saw_start, true)) {
        return true;
    }
    return !saw_start;
}

static int kmpp_ioctl(int fd, uint32_t cmd, uint32_t ctrl, void *data, uint32_t size)
{
    struct kmpp_vcodec_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.cmd = cmd;
    msg.ctrl_cmd = ctrl;
    msg.size = size;
    msg.data_ptr = (uint64_t)(uintptr_t)data;
    return ioctl(fd, KMPP_VOCDEC_IOC_CFG, &msg);
}

static int kmpp_drain_packets(struct rk_encoder *enc, int timeout_ms)
{
    struct kmpp_venc_packet pkt;
    struct pollfd pfds[2] = {
        {.fd = enc->vcodec_fd, .events = POLLIN},
        {.fd = enc->drain_wake, .events = POLLIN},
    };
    int got = 0;
    static unsigned logged_fail;
    static unsigned logged_au;
    static unsigned logged_wrap;
    static int dumped_poison;

    if (enc->vcodec_fd < 0 || enc->stream_cap == 0) {
        return 0;
    }
    int ready = poll(pfds, 2, timeout_ms);
    if (pfds[1].revents & POLLIN) {
        clear_wake_fd(enc->drain_wake);
    }
    if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return -1;
    }
    if (enc->stop || atomic_load(&enc->drain_stop) || ready <= 0 ||
        !(pfds[0].revents & POLLIN)) {
        return 0;
    }
    for (;;) {
        int ir;
        int slab;
        uint8_t *dst;
        uint32_t user_cap = (uint32_t)enc->stream_cap;
        uint32_t ring_size;
        uint32_t start_off;
        bool drop;
        bool picture;
        bool wrapped;
        bool poison;
        uint64_t encoded_ns;

        pthread_mutex_lock(&enc->mutex);
        slab = au_slab_acquire_locked(enc);
        enc->slab_wait = slab < 0;
        if (slab < 0) {
            enc->slab_waits++;
        }
        pthread_mutex_unlock(&enc->mutex);
        if (slab < 0) {
            /* Every slab is queued or borrowed. Leave the AU in the kernel:
             * the frame stays in flight, capture skips CIF frames before
             * IN_FRM and no encoded reference is lost. A released slab wakes
             * this wait. */
            struct pollfd wait = {.fd = enc->drain_wake, .events = POLLIN};
            (void)poll(&wait, 1, RK_PACKET_WAIT_MS);
            pthread_mutex_lock(&enc->mutex);
            enc->slab_wait = false;
            pthread_mutex_unlock(&enc->mutex);
            break;
        }
        dst = enc->au_slab[slab];
        memset(&pkt, 0, sizeof(pkt));
        pkt.u64packet_addr = (uint64_t)(uintptr_t)dst;
        pkt.buf_size = user_cap;
        ir = kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_OUT_STRM_BUF_RDY, 0, &pkt,
                        (uint32_t)sizeof(pkt));
        /* Encode-output time for the AU (BOUND_ENCODED_TIME); pkt.u64pts is the
         * input frame's time, not when VEPU produced this packet. */
        encoded_ns = monotonic_ns();
        if (ir != 0 || pkt.len == 0 || pkt.len > user_cap) {
            pthread_mutex_lock(&enc->mutex);
            au_slab_release_locked(enc, slab);
            pthread_mutex_unlock(&enc->mutex);
            if (timeout_ms > 0 && ir == 0 && pkt.len > user_cap && logged_fail < 4) {
                fprintf(stderr, "rk-mpp: get_stream bad len=%u cap=%u\n", pkt.len, user_cap);
                logged_fail++;
            }
            break;
        }
        /* JetKVM GetStream forwards every pack. Do not filter NALs here. */
        ring_size = pkt.buf_size;
        start_off = pkt.offset;
        wrapped = ring_size != 0 &&
                  (uint64_t)start_off + (uint64_t)pkt.len > (uint64_t)ring_size;
        picture = au_is_picture(dst, pkt.len);
        poison = !picture;
        if (logged_au < 8 || (wrapped && logged_wrap < 8) || pkt.len > 40000u) {
            fprintf(stderr,
                    "rk-mpp: au len=%u off=%u ring=%u wrap=%d pic=%d flag=0x%x nal=%02x\n",
                    pkt.len, start_off, ring_size, wrapped ? 1 : 0, picture ? 1 : 0,
                    pkt.flag, pkt.len > 4 ? dst[4] : 0);
            logged_au++;
            if (wrapped) {
                logged_wrap++;
            }
        }
        if (poison && !dumped_poison) {
            FILE *dump = fopen("/var/tmp/onekvm/wrap-au.bin", "wb");

            if (dump) {
                (void)fwrite(dst, 1, pkt.len, dump);
                (void)fclose(dump);
            }
            dumped_poison = 1;
        }
        pthread_mutex_lock(&enc->mutex);
        /* Drop leftovers only while VEPU has no submitted frame. After IN_FRM,
         * keep the AU even if skip_au is still set — discarding that IDR
         * freezes Chrome at 0 FPS until the next GOP. */
        drop = enc->drain_pause || (enc->skip_au && enc->in_flight == 0);
        if (drop) {
            au_slab_release_locked(enc, slab);
            pthread_mutex_unlock(&enc->mutex);
        } else {
            queue_push_slab_locked(enc, slab, pkt.len,
                                   (pkt.flag & KMPP_PACKET_FLAG_INTRA) != 0 ||
                                       au_is_keyframe(dst, pkt.len),
                                   encoded_ns);
            if (!enc->have_au) {
                fprintf(stderr, "rk-mpp: first VEPU AU len=%u pic=%d\n", pkt.len,
                        picture);
            }
            enc->have_au = true;
            pthread_mutex_unlock(&enc->mutex);
        }
        (void)kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_OUT_STRM_END, 0, &pkt,
                         (uint32_t)sizeof(pkt));
        if (!drop) {
            pthread_mutex_lock(&enc->mutex);
            if (enc->in_flight > 0) {
                enc->in_flight--;
            }
            enc->au_done++;
            latency_submit_complete_locked(enc, monotonic_ns());
            pthread_cond_broadcast(&enc->ready);
            pthread_mutex_unlock(&enc->mutex);
            wake_fd(enc->capture_wake);
        }
        got++;
        pfds[0].revents = 0;
        if (poll(pfds, 1, 0) <= 0 || !(pfds[0].revents & POLLIN)) {
            break;
        }
    }
    return got;
}

static void *kmpp_drain_thread(void *arg)
{
    struct rk_encoder *enc = arg;

    while (!enc->stop && !atomic_load(&enc->drain_stop)) {
        if (kmpp_drain_packets(enc, RK_DRAIN_POLL_MS) < 0) {
            break;
        }
    }
    return NULL;
}

static int kmpp_alloc_dma(int vcodec_fd, uint32_t size)
{
    struct kmpp_dma_alloc alloc;

    memset(&alloc, 0, sizeof(alloc));
    alloc.size = size;
    if (kmpp_ioctl(vcodec_fd, KMPP_VCODEC_CHAN_ONEKVM_ALLOC_DMA, 0, &alloc,
                   (uint32_t)sizeof(alloc)) != 0 ||
        alloc.fd < 0) {
        return -1;
    }
    return alloc.fd;
}

static int kmpp_alloc_dma_fd(struct rk_encoder *enc, uint32_t size)
{
    return kmpp_alloc_dma(enc->vcodec_fd, size);
}

static void kmpp_close_fds(int *fds, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

static void display_unmap(void **maps, size_t *lens, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (maps[i] != NULL && maps[i] != MAP_FAILED && lens[i] > 0) {
            munmap(maps[i], lens[i]);
        }
        maps[i] = NULL;
        lens[i] = 0;
    }
}

static void display_map_fd(void **maps, size_t *lens, unsigned index, int fd, size_t length)
{
    if (index >= ONEKVM_RK_MPP_BUFFER_COUNT) {
        return;
    }
    if (maps[index] != NULL && maps[index] != MAP_FAILED && lens[index] > 0) {
        munmap(maps[index], lens[index]);
    }
    maps[index] = NULL;
    lens[index] = 0;
    if (fd < 0 || length == 0) {
        return;
    }
    maps[index] = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
    if (maps[index] == MAP_FAILED) {
        maps[index] = NULL;
        return;
    }
    lens[index] = length;
}

static void display_remap_all(void **maps, size_t *lens, const int *fds, unsigned n, size_t length)
{
    unsigned i;

    display_unmap(maps, lens, ONEKVM_RK_MPP_BUFFER_COUNT);
    for (i = 0; i < n && i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
        display_map_fd(maps, lens, i, fds[i], length);
    }
}

static uint32_t rk_align16(uint32_t v)
{
    return (v + 15u) & ~15u;
}

static uint32_t rk_ver_stride(uint32_t fourcc, uint32_t height)
{
    /* CIF NV16/NV12 UV sits at bytesperline*height. VEPU UV is
     * hor_stride*ver_stride; aligning 1080 to 1088 shifts chroma 8
     * lines and leaves a Y=U=V=0 green bar. Packed YUYV has no UV
     * plane — keep the 16-aligned DMA height. */
    if (fourcc == V4L2_PIX_FMT_NV16 || fourcc == V4L2_PIX_FMT_NV12 ||
        fourcc == V4L2_PIX_FMT_NV61 || fourcc == V4L2_PIX_FMT_NV21) {
        return height;
    }
    return rk_align16(height);
}

/* After DQBUF, put the VEPU CMA fd back. Some CIF DQBUF paths leave m.fd empty. */
static void qbuf_restore_dmabuf(struct v4l2_buffer *buf, struct v4l2_plane *pl, const int *dma_fds,
                                uint32_t plane_len)
{
    buf->length = 1;
    buf->m.planes = pl;
    if (buf->memory == V4L2_MEMORY_DMABUF && buf->index < ONEKVM_RK_MPP_BUFFER_COUNT &&
        dma_fds[buf->index] >= 0) {
        pl->m.fd = dma_fds[buf->index];
        if (pl->length == 0) {
            pl->length = plane_len;
        }
    }
}

static int capture_qbuf_index(int fd, uint32_t mem, const int *dma_fds, uint32_t plane_len,
                              unsigned index, uint64_t *qbuf_ns)
{
    struct v4l2_buffer buf;
    struct v4l2_plane pl;

    memset(&buf, 0, sizeof(buf));
    memset(&pl, 0, sizeof(pl));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = mem;
    buf.index = index;
    qbuf_restore_dmabuf(&buf, &pl, dma_fds, plane_len);
    if (xioctl(fd, VIDIOC_QBUF, &buf) != 0) {
        return -1;
    }
    note_slot_qbuf(qbuf_ns, index);
    return 0;
}

struct rk_cif_hold {
    void *source;
    unsigned spare[ONEKVM_RK_MPP_BUFFER_COUNT];
    unsigned spare_n;
    unsigned cif_n;
    unsigned pending[ONEKVM_RK_MPP_BUFFER_COUNT];
    unsigned pend_head;
    unsigned pend_n;
    unsigned released;
    uint64_t qbuf_ns[ONEKVM_RK_MPP_BUFFER_COUNT];
};

static void cif_hold_reset(struct rk_cif_hold *hold)
{
    if (hold != NULL) {
        memset(hold, 0, sizeof(*hold));
    }
}

static void cif_spare_push(struct rk_cif_hold *hold, unsigned idx)
{
    if (hold != NULL && hold->spare_n < ONEKVM_RK_MPP_BUFFER_COUNT) {
        hold->spare[hold->spare_n++] = idx;
    }
}

static void cif_feed(int fd, uint32_t mem, const int *dma_fds, uint32_t plane_len,
                    struct rk_cif_hold *hold)
{
    while (hold != NULL && hold->cif_n < RK_CIF_KEEP) {
        int idx = -1;
        unsigned candidate;

        for (candidate = 0; candidate < hold->spare_n; candidate++) {
            unsigned slot = hold->spare[candidate];
            if (!rk_snapshot_holds(hold->source, dma_fds[slot])) {
                idx = (int)slot;
                hold->spare[candidate] = hold->spare[--hold->spare_n];
                break;
            }
        }

        if (idx < 0) {
            break;
        }
        if (capture_qbuf_index(fd, mem, dma_fds, plane_len, (unsigned)idx, hold->qbuf_ns) != 0) {
            cif_spare_push(hold, (unsigned)idx);
            break;
        }
        hold->cif_n++;
    }
}

static void cif_recycle(int fd, uint32_t mem, const int *dma_fds, uint32_t plane_len,
                       struct rk_cif_hold *hold, unsigned idx)
{
    cif_spare_push(hold, idx);
    cif_feed(fd, mem, dma_fds, plane_len, hold);
}

static int capture_prime_cif(void *source, int fd, uint32_t mem, const int *dma_fds, uint32_t plane_len,
                             unsigned allocated, struct rk_cif_hold *hold)
{
    unsigned i;

    if (hold == NULL) {
        return -1;
    }
    cif_hold_reset(hold);
    hold->source = source;
    for (i = 0; i < allocated && i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
        if (hold->cif_n < RK_CIF_KEEP) {
            if (capture_qbuf_index(fd, mem, dma_fds, plane_len, i, hold->qbuf_ns) != 0) {
                return -1;
            }
            hold->cif_n++;
        } else {
            cif_spare_push(hold, i);
        }
    }
    return 0;
}

/* CIF must not reuse a DMABUF until VEPU has emitted the matching AU.
 * Immediate QBUF after IN_FRM overwrites the frame mid-encode: motion tears
 * and P-frames ghost. Hold the encoded slot until the AU, then park it on
 * the spare list and only keep RK_CIF_KEEP buffers in the driver. */
static void capture_release_encoded(struct rk_encoder *enc, int fd, uint32_t mem,
                                    const int *dma_fds, uint32_t plane_len,
                                    struct rk_cif_hold *hold)
{
    unsigned done;

    pthread_mutex_lock(&enc->mutex);
    done = enc->au_done;
    pthread_mutex_unlock(&enc->mutex);
    while (hold->released + RK_RELEASE_LAG < done && hold->pend_n > 0) {
        unsigned idx = hold->pending[hold->pend_head];

        cif_spare_push(hold, idx);
        hold->pend_head = (hold->pend_head + 1u) % ONEKVM_RK_MPP_BUFFER_COUNT;
        hold->pend_n--;
        hold->released++;
    }
    cif_feed(fd, mem, dma_fds, plane_len, hold);
}

/* Keep at most RK_MAX_INFLIGHT outstanding encodes. Never QBUF a slot VEPU
 * still holds — that overwrites the source mid-encode and motion tears. */
static void capture_reclaim_done(struct rk_encoder *enc, int fd, uint32_t mem,
                                 const int *dma_fds, uint32_t plane_len,
                                 struct rk_cif_hold *hold)
{
    capture_release_encoded(enc, fd, mem, dma_fds, plane_len, hold);
}

static void capture_wait_au(struct rk_encoder *enc, unsigned released)
{
    struct timespec deadline;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += RK_WAIT_AU_NS;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&enc->mutex);
    /* Wait for any new picture AU. Do not add RELEASE_LAG here: that
     * deadlocks when pend_n is already at MAX_INFLIGHT (no extra IN_FRM
     * can produce the AU that would satisfy released+LAG). */
    while (!enc->stop && enc->au_done <= released) {
        if (pthread_cond_timedwait(&enc->ready, &enc->mutex, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&enc->mutex);
}

static void capture_reset_inflight(struct rk_encoder *enc, struct rk_cif_hold *hold)
{
    pthread_mutex_lock(&enc->mutex);
    /* A stopped CIF can leave one or more old AUs in the output queue. Drop
     * them before the no-signal frame, then allow a new IDR when the capture
     * stream returns. */
    queue_flush_locked(enc);
    enc->have_au = false;
    enc->au_done = 0;
    enc->in_flight = 0;
    enc->skip_au = true;
    latency_submit_reset_locked(enc);
    pthread_mutex_unlock(&enc->mutex);
    if (hold != NULL) {
        hold->pend_head = 0;
        hold->pend_n = 0;
        hold->released = 0;
    }
}

static void capture_wait_vepu_idle(struct rk_encoder *enc)
{
    struct timespec deadline;

    pthread_mutex_lock(&enc->mutex);
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == 0) {
        deadline.tv_nsec += 120000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        while (!enc->stop && enc->in_flight > 0) {
            if (pthread_cond_timedwait(&enc->ready, &enc->mutex, &deadline) == ETIMEDOUT) {
                break;
            }
        }
    }
    pthread_mutex_unlock(&enc->mutex);
}

static uint32_t kmpp_fmt_from_fourcc(uint32_t fourcc)
{
    if (fourcc == V4L2_PIX_FMT_NV12) {
        return KMPP_FMT_YUV420SP;
    }
    if (fourcc == V4L2_PIX_FMT_NV16) {
        return KMPP_FMT_YUV422SP;
    }
    if (fourcc == V4L2_PIX_FMT_UYVY) {
        return KMPP_FMT_YUV422_UYVY;
    }
    return KMPP_FMT_YUV422_YUYV;
}

static void kmpp_close_encoder(struct rk_encoder *enc)
{
    int fd;

    pthread_mutex_lock(&enc->mutex);
    fd = enc->vcodec_fd;
    enc->vcodec_fd = -1;
    pthread_mutex_unlock(&enc->mutex);
    if (fd >= 0) {
        (void)kmpp_ioctl(fd, KMPP_VCODEC_CHAN_STOP, 0, NULL, 0);
        /* vcodec_close destroys a created channel. An explicit DESTROY
         * followed by close can deinit the same channel twice during a mode
         * switch. */
        close(fd);
    }
    rk_rga_dma_close(enc->scale);
    enc->scale = NULL;
    enc->scaled = false;
    if (enc->scale_fd >= 0) {
        close(enc->scale_fd);
        enc->scale_fd = -1;
    }
    if (enc->ns_map != NULL) {
        munmap(enc->ns_map, enc->ns_bytes);
        enc->ns_map = NULL;
    }
    if (enc->ns_fd >= 0) {
        close(enc->ns_fd);
        enc->ns_fd = -1;
    }
    enc->ns_bytes = 0;
    enc->ns_channel = false;
    enc->ns_drawn = false;
}

/* Set up RGA output scaling for a new VEPU channel on vcodec_fd. Any
 * failure keeps the native size: an output size the current input cannot
 * produce (the HDMI mode changed after source_reset) or a broken RGA must
 * not leave the binding without an encoder. */
static bool kmpp_open_scale(struct rk_encoder *enc, int vcodec_fd, uint32_t fourcc, int width,
                            int height, uint32_t hor_stride, int scaled, int dw, int dh)
{
    const char *reason = NULL;

    if (scaled < 0) {
        reason = "not a supported downscale of the input";
    } else if (enc->scale_disabled) {
        reason = "RGA failed on this binding";
    } else {
        enc->scale = rk_rga_dma_open(fourcc, width, height, hor_stride, dw, dh,
                                     &enc->scale_bytes);
        if (enc->scale == NULL) {
            reason = "RGA unavailable";
        } else {
            enc->scale_fd = kmpp_alloc_dma(
                vcodec_fd, (uint32_t)onekvm_nv12_dma_bytes(dw, dh, enc->scale_bytes));
            if (enc->scale_fd < 0) {
                rk_rga_dma_close(enc->scale);
                enc->scale = NULL;
                reason = "no CMA for the scaled frame";
            }
        }
    }
    if (reason != NULL) {
        fprintf(stderr, "rk-mpp: output %dx%d unavailable (%s), encoding %dx%d\n", dw, dh,
                reason, width, height);
        return false;
    }
    enc->scaled = true;
    enc->scale_failures = 0;
    return true;
}

/* A failed RGA job leaves its context unusable: reopen it in place. After
 * RK_SCALE_MAX_FAILURES in a row, or if reopening fails, disable scaling for
 * this binding; the capture loop then reopens VEPU at the native size. */
static void scale_recover(struct rk_encoder *enc, uint32_t fourcc, int width, int height,
                          uint32_t hor_stride)
{
    size_t bytes = 0;

    rk_rga_dma_close(enc->scale);
    enc->scale = NULL;
    if (++enc->scale_failures < RK_SCALE_MAX_FAILURES) {
        enc->scale = rk_rga_dma_open(fourcc, width, height, hor_stride, enc->active_width,
                                     enc->active_height, &bytes);
        if (enc->scale != NULL && bytes == enc->scale_bytes) {
            return;
        }
        rk_rga_dma_close(enc->scale);
        enc->scale = NULL;
    }
    enc->scale_disabled = true;
    fprintf(stderr, "rk-mpp: RGA scale failed %u times, reopening VEPU at %dx%d\n",
            enc->scale_failures, width, height);
}

/* Signal the bridge's YCbCr range and matrix in the VUI, unless the caller
 * set them. kmpp writes video_signal_type for full range, or for any range
 * with a matrix. A kernel without the colour fields gets the short struct and
 * the stream stays unspecified. */
static int kmpp_setup_channel(struct rk_encoder *enc, int fd, struct kmpp_onekvm_setup *setup)
{
    uint32_t encoding;
    uint32_t quantization;
    uint32_t range = ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED;

    if (setup->range == 0 &&
        rk_source_ycbcr(enc->bound_source, &encoding, &quantization) == 0) {
        setup->range =
            quantization == V4L2_QUANTIZATION_FULL_RANGE ? KMPP_RANGE_JPEG : KMPP_RANGE_MPEG;
        setup->matrix = encoding == V4L2_YCBCR_ENC_601   ? KMPP_SPC_SMPTE170M
                        : encoding == V4L2_YCBCR_ENC_709 ? KMPP_SPC_BT709
                                                         : 0u;
    }
    if (kmpp_ioctl(fd, KMPP_VCODEC_CHAN_ONEKVM_SETUP, 0, setup, (uint32_t)sizeof(*setup)) == 0) {
        if (setup->range == KMPP_RANGE_JPEG) {
            range = ONEKVM_VIDEO_COLOR_RANGE_FULL;
        } else if (setup->range == KMPP_RANGE_MPEG && setup->matrix != 0) {
            range = ONEKVM_VIDEO_COLOR_RANGE_LIMITED;
        }
    } else if (kmpp_ioctl(fd, KMPP_VCODEC_CHAN_ONEKVM_SETUP, 0, setup,
                          (uint32_t)KMPP_ONEKVM_SETUP_V1_SIZE) != 0) {
        return -1;
    }
    atomic_store(&enc->stream_range, range);
    return 0;
}

/* no_signal opens the channel for the cached no-signal frame: already at the
 * output size, BT.601 limited range like the cache, and a fixed GOP. */
static int kmpp_open_encoder_inner(struct rk_encoder *enc, int width, int height, int fps,
                                   uint32_t fourcc, uint32_t hor_stride, bool no_signal)
{
    struct kmpp_vcodec_attr attr;
    struct kmpp_onekvm_setup setup;
    uint32_t coding;
    int fd;
    int dw = width;
    int dh = height;
    int scaled =
        no_signal ? 0 : rk_source_output_size(enc->bound_source, width, height, &dw, &dh);

    fd = open("/dev/vcodec", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (scaled != 0 &&
        kmpp_open_scale(enc, fd, fourcc, width, height, hor_stride, scaled, dw, dh)) {
        width = dw;
        height = dh;
        fourcc = V4L2_PIX_FMT_NV12;
        hor_stride = (uint32_t)dw;
    }
    memset(&attr, 0, sizeof(attr));
    coding = enc->config.codec == ONEKVM_VIDEO_CODEC_H265 ? KMPP_VIDEO_CODING_HEVC
                                                          : KMPP_VIDEO_CODING_AVC;
    attr.type = KMPP_CTX_ENC;
    attr.coding = (int32_t)coding;
    attr.chan_id = 0;
    attr.buf_size = (uint32_t)width * (uint32_t)height * 3u / 2u;
    if (attr.buf_size < 256u * 1024u) {
        attr.buf_size = 256u * 1024u;
    }
    attr.max_strm_cnt = 3;
    attr.max_width = 1920;
    attr.max_height = 1080;
    if (kmpp_ioctl(fd, KMPP_VCODEC_CHAN_CREATE, 0, &attr, (uint32_t)sizeof(attr)) != 0) {
        close(fd);
        return -1;
    }
    memset(&setup, 0, sizeof(setup));
    setup.width = (uint32_t)width;
    setup.height = (uint32_t)height;
    setup.hor_stride = hor_stride;
    setup.ver_stride = rk_ver_stride(fourcc, (uint32_t)height);
    setup.fmt = kmpp_fmt_from_fourcc(fourcc);
    setup.fps = fps > 0 ? (uint32_t)fps : 30u;
    setup.bps = (uint32_t)onekvm_target_bitrate_kbps(width, height, enc->config.quality_factor,
                                                     enc->config.bitrate_kbps) *
                1000u;
    setup.gop = no_signal ? RK_NS_GOP : (uint32_t)onekvm_auto_gop(enc->config.gop, fps);
    setup.coding = coding;
    if (no_signal) {
        setup.range = KMPP_RANGE_MPEG;
        setup.matrix = KMPP_SPC_SMPTE170M;
    }
    if (kmpp_setup_channel(enc, fd, &setup) != 0 ||
        kmpp_ioctl(fd, KMPP_VCODEC_CHAN_START, 0, NULL, 0) != 0) {
        close(fd);
        return -1;
    }
    /* Keep the AU slab addresses stable across 1080p/BIOS mode changes: Core
     * may still hold a borrowed packet while the encoder is being reopened. */
    enc->stream_cap = RK_MAX_STREAM_BYTES;
    {
        unsigned si;

        for (si = 0; si < RK_AU_SLABS; si++) {
            if (enc->au_slab[si] == NULL) {
                enc->au_slab[si] = malloc(enc->stream_cap);
            }
            if (enc->au_slab[si] == NULL) {
                (void)kmpp_ioctl(fd, KMPP_VCODEC_CHAN_STOP, 0, NULL, 0);
                close(fd);
                return -1;
            }
        }
    }
    free(enc->stream_buf);
    enc->stream_buf = NULL;
    pthread_mutex_lock(&enc->mutex);
    enc->vcodec_fd = fd;
    enc->active_width = width;
    enc->active_height = height;
    {
        struct kmpp_onekvm_rate rate = {
            .bps = (uint32_t)onekvm_target_bitrate_kbps(width, height,
                        enc->config.quality_factor, enc->config.bitrate_kbps) * 1000u,
            .initial_qp = enc->config.initial_qp,
            .min_qp = enc->config.min_qp,
            .max_qp = enc->config.max_qp,
        };
        if (kmpp_ioctl(fd, KMPP_VCODEC_CHAN_ONEKVM_RATE, 0, &rate, sizeof(rate)) != 0) {
            fprintf(stderr, "rk-mpp: initial rate update failed errno=%d\n", errno);
        }
    }
    pthread_mutex_unlock(&enc->mutex);
    (void)kmpp_ioctl(fd, KMPP_VCODEC_CHAN_CONTROL, KMPP_ENC_SET_IDR_FRAME, NULL, 0);
    fprintf(stderr,
            "rk-mpp: kmpp open %dx%d stride=%u ver=%u fmt=%u enc_fps=%u bps=%u pkt=%zu frame=%zu\n",
            width, height, hor_stride, setup.ver_stride, setup.fmt, setup.fps, setup.bps,
            sizeof(struct kmpp_venc_packet), sizeof(struct kmpp_frame_infos));
    return 0;
}

static int kmpp_open_encoder(struct rk_encoder *enc, int width, int height, int fps,
                             uint32_t fourcc, uint32_t hor_stride, bool no_signal)
{
    int result = kmpp_open_encoder_inner(enc, width, height, fps, fourcc, hor_stride, no_signal);
    if (result != 0) {
        kmpp_close_encoder(enc);
    }
    return result;
}

static void kmpp_drain_stop(struct rk_encoder *enc)
{
    atomic_store(&enc->drain_stop, true);
    wake_fd(enc->drain_wake);
    if (enc->drain_started) {
        pthread_join(enc->drain_thread, NULL);
        enc->drain_started = false;
    }
    atomic_store(&enc->drain_stop, false);
}

static void kmpp_drain_start(struct rk_encoder *enc)
{
    pthread_mutex_lock(&enc->mutex);
    enc->drain_pause = false;
    pthread_mutex_unlock(&enc->mutex);
    atomic_store(&enc->drain_stop, false);
    if (enc->vcodec_fd >= 0 && !enc->drain_started) {
        if (pthread_create(&enc->drain_thread, NULL, kmpp_drain_thread, enc) == 0) {
            enc->drain_started = true;
        } else {
            fprintf(stderr, "rk-mpp: drain thread failed errno=%d\n", errno);
        }
    }
}

static void dma_buf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = {.flags = flags | DMA_BUF_SYNC_WRITE};

    (void)xioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* The no-signal frame uses the requested output size, else the last capture
 * size. */
static void ns_size(struct rk_encoder *enc, int width, int height, int *out_w, int *out_h)
{
    int dw = width;
    int dh = height;

    (void)rk_source_output_size(enc->bound_source, width, height, &dw, &dh);
    if (dw < 320 || dh < 200 || dw > 1920 || dh > 1080 || (dw & 1) || (dh & 1)) {
        dw = width;
        dh = height;
    }
    if (dw < 320 || dh < 200 || dw > 1920 || dh > 1080 || (dw & 1) || (dh & 1)) {
        dw = 1920;
        dh = 1080;
    }
    *out_w = dw;
    *out_h = dh;
}

/* Replace the open channel with one that encodes the no-signal frame. */
static int ns_open(struct rk_encoder *enc, int width, int height)
{
    uint32_t stride = rk_align16((uint32_t)width);
    size_t bytes = onekvm_nv12_dma_bytes((int)stride, height, 0);
    void *map;

    kmpp_drain_stop(enc);
    capture_reset_inflight(enc, NULL);
    kmpp_close_encoder(enc);
    if (kmpp_open_encoder(enc, width, height, RK_NS_FPS, V4L2_PIX_FMT_NV12, stride, true) != 0) {
        fprintf(stderr, "rk-mpp: no-signal encoder %dx%d failed errno=%d\n", width, height,
                errno);
        return -1;
    }
    enc->ns_fd = kmpp_alloc_dma(enc->vcodec_fd, (uint32_t)bytes);
    map = enc->ns_fd >= 0
              ? mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, enc->ns_fd, 0)
              : MAP_FAILED;
    if (map == MAP_FAILED) {
        fprintf(stderr, "rk-mpp: no-signal frame %dx%d alloc failed errno=%d\n", width, height,
                errno);
        kmpp_close_encoder(enc);
        return -1;
    }
    enc->ns_map = map;
    enc->ns_bytes = bytes;
    enc->ns_stride = stride;
    enc->ns_channel = true;
    enc->ns_drawn = false;
    kmpp_drain_start(enc);
    return 0;
}

/* Encode the no-signal frame once more: redraw it when the cache changed,
 * and stop while no reader is active, as live capture does. */
static void ns_submit(struct rk_encoder *enc, uint64_t now)
{
    struct kmpp_frame_infos info;
    int width = enc->active_width;
    int height = enc->active_height;
    bool idr;

    pthread_mutex_lock(&enc->mutex);
    if (now - enc->ns_submit_ns < RK_PLACEHOLDER_NS ||
        (enc->in_flight > 0 && now - enc->ns_submit_ns < RK_NS_RETRY_NS)) {
        pthread_mutex_unlock(&enc->mutex);
        return;
    }
    if (enc->in_flight > 0) {
        /* VEPU never returned the last frame: count it lost, not pending. */
        enc->in_flight = 0;
        latency_submit_reset_locked(enc);
        enc->force_keyframe = true;
    }
    if (!reader_active(enc->last_read_ns, now)) {
        if (!enc->vepu_paused) {
            queue_flush_locked(enc);
            enc->vepu_paused = true;
        }
        pthread_mutex_unlock(&enc->mutex);
        return;
    }
    if (enc->vepu_paused) {
        enc->vepu_paused = false;
        enc->force_keyframe = true;
    }
    pthread_mutex_unlock(&enc->mutex);
    if (!enc->ns_drawn || now - enc->ns_checked_ns >= RK_NS_CHECK_NS) {
        struct rk_no_signal_token token;

        enc->ns_checked_ns = now;
        rk_no_signal_token(width, height, &token);
        if (!enc->ns_drawn || memcmp(&token, &enc->ns_token, sizeof(token)) != 0) {
            dma_buf_sync(enc->ns_fd, DMA_BUF_SYNC_START);
            (void)rk_no_signal_render_nv12(enc->ns_map, enc->ns_bytes, width, height,
                                           enc->ns_stride, (uint32_t)height);
            dma_buf_sync(enc->ns_fd, DMA_BUF_SYNC_END);
            enc->ns_token = token;
            enc->ns_drawn = true;
            pthread_mutex_lock(&enc->mutex);
            enc->force_keyframe = true;
            pthread_mutex_unlock(&enc->mutex);
        }
    }
    memset(&info, 0, sizeof(info));
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.hor_stride = enc->ns_stride;
    info.ver_stride = (uint32_t)height;
    info.hor_stride_pixel = (uint32_t)width;
    info.fmt = KMPP_FMT_YUV420SP;
    info.fd = (uint32_t)enc->ns_fd;
    info.pts = now / 1000ull;
    info.jpeg_chan_id = -1;
    pthread_mutex_lock(&enc->mutex);
    idr = enc->force_keyframe;
    enc->force_keyframe = false;
    enc->skip_au = false;
    enc->in_flight++;
    latency_submit_push_locked(enc, monotonic_ns());
    pthread_mutex_unlock(&enc->mutex);
    if (idr) {
        (void)kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_CONTROL, KMPP_ENC_SET_IDR_FRAME, NULL,
                         0);
    }
    enc->ns_submit_ns = now;
    if (kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_IN_FRM_RDY, 0, &info,
                   (uint32_t)sizeof(info)) != 0) {
        pthread_mutex_lock(&enc->mutex);
        if (enc->in_flight > 0) {
            enc->in_flight--;
        }
        latency_submit_pop_tail_locked(enc);
        enc->force_keyframe = enc->force_keyframe || idr;
        pthread_mutex_unlock(&enc->mutex);
    }
}

/* While capture is stopped: keep a no-signal channel at the output size
 * open and feed it. Opening is retried at most once per RK_NS_RETRY_NS. */
static void ns_tick(struct rk_encoder *enc, int width, int height, uint64_t now)
{
    int nw;
    int nh;

    ns_size(enc, width, height, &nw, &nh);
    if (!enc->ns_channel || enc->active_width != nw || enc->active_height != nh) {
        if (enc->ns_retry_ns != 0 && now - enc->ns_retry_ns < RK_NS_RETRY_NS) {
            return;
        }
        enc->ns_retry_ns = now;
        if (ns_open(enc, nw, nh) != 0) {
            return;
        }
        enc->ns_retry_ns = 0;
    }
    ns_submit(enc, now);
}

/* Capture is not available for this binding: show the no-signal frame
 * until unbind. */
static void ns_until_stop(struct rk_encoder *enc, int width, int height)
{
    while (!enc->stop) {
        ns_tick(enc, width, height, monotonic_ns());
        usleep(20000);
    }
    kmpp_drain_stop(enc);
    kmpp_close_encoder(enc);
}

/* Stop feeding the no-signal channel but keep it open: capture_restart_stream
 * then reopens VEPU for capture, or restarts the drain if capture fails. */
static void ns_park(struct rk_encoder *enc)
{
    if (enc->ns_channel) {
        kmpp_drain_stop(enc);
        capture_reset_inflight(enc, NULL);
        pthread_mutex_lock(&enc->mutex);
        enc->drain_pause = true;
        pthread_mutex_unlock(&enc->mutex);
    }
}

/* Capture takes over VEPU from the no-signal channel. */
static void ns_close(struct rk_encoder *enc)
{
    if (enc->ns_channel) {
        kmpp_drain_stop(enc);
        capture_reset_inflight(enc, NULL);
        kmpp_close_encoder(enc);
    }
}
#endif

#ifdef ONEKVM_HAVE_ROCKIT
static bool au_is_keyframe(const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            uint8_t nal = data[i + 4];
            uint8_t type = nal & 0x1fu;
            if (type == 5 || type == 7 || type == 32 || type == 19 || type == 20) {
                return true;
            }
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            uint8_t type = data[i + 3] & 0x1fu;
            if (type == 5 || type == 7) {
                return true;
            }
        }
    }
    return false;
}
#endif

#ifdef ONEKVM_HAVE_ROCKIT
static int venc_create(struct rk_encoder *enc, int width, int height, int fps)
{
    VENC_CHN_ATTR_S attr;
    VENC_RECV_PIC_PARAM_S recv;
    int bitrate = onekvm_bitrate_kbps(width, height, enc->config.quality_factor);
    int max_bitrate = bitrate * 3 / 2;
    int min_bitrate = bitrate / 2;
    int gop = onekvm_auto_gop(enc->config.gop, fps);
    uint32_t codec = onekvm_resolved_codec(enc->config.codec);
    int ret;

    if (min_bitrate < 2) {
        min_bitrate = 2;
    }
    if (fps < 1) {
        fps = 30;
    }
    memset(&attr, 0, sizeof(attr));
    if (codec == ONEKVM_VIDEO_CODEC_H265) {
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
        attr.stRcAttr.stH265Vbr.u32BitRate = (RK_U32)bitrate;
        attr.stRcAttr.stH265Vbr.u32MaxBitRate = (RK_U32)max_bitrate;
        attr.stRcAttr.stH265Vbr.u32MinBitRate = (RK_U32)min_bitrate;
        attr.stRcAttr.stH265Vbr.u32Gop = (RK_U32)gop;
        attr.stRcAttr.stH265Vbr.u32StatTime = 2;
        attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum = (RK_U32)fps;
        attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen = 1;
        attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum = (RK_U32)fps;
        attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen = 1;
        attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
        attr.stVencAttr.u32Profile = H265E_PROFILE_MAIN;
    } else {
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
        attr.stRcAttr.stH264Vbr.u32BitRate = (RK_U32)bitrate;
        attr.stRcAttr.stH264Vbr.u32MaxBitRate = (RK_U32)max_bitrate;
        attr.stRcAttr.stH264Vbr.u32MinBitRate = (RK_U32)min_bitrate;
        attr.stRcAttr.stH264Vbr.u32Gop = (RK_U32)gop;
        attr.stRcAttr.stH264Vbr.u32StatTime = 2;
        attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum = (RK_U32)fps;
        attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen = 1;
        attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum = (RK_U32)fps;
        attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen = 1;
        attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
        attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    }
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV422_YUYV;
    attr.stVencAttr.u32PicWidth = (RK_U32)width;
    attr.stVencAttr.u32PicHeight = (RK_U32)height;
    attr.stVencAttr.u32VirWidth = ((uint32_t)width + 15u) & ~15u;
    attr.stVencAttr.u32VirHeight = ((uint32_t)height + 15u) & ~15u;
    attr.stVencAttr.u32StreamBufCnt = 3;
    attr.stVencAttr.u32BufSize = (RK_U32)width * (RK_U32)height * 3u / 2u;
    attr.stVencAttr.enMirror = MIRROR_NONE;
    ret = RK_MPI_VENC_CreateChn(enc->venc_chn, &attr);
    if (ret != RK_SUCCESS) {
        return ret;
    }
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;
    return RK_MPI_VENC_StartRecvFrame(enc->venc_chn, &recv);
}

static void *venc_reader(void *arg)
{
    struct rk_encoder *enc = arg;
    VENC_STREAM_S stream;
    VENC_PACK_S pack;

    memset(&stream, 0, sizeof(stream));
    memset(&pack, 0, sizeof(pack));
    stream.pstPack = &pack;
    while (!enc->stop) {
        int ret = RK_MPI_VENC_GetStream(enc->venc_chn, &stream, 200);
        if (ret == RK_SUCCESS) {
            uint64_t encoded_ns = monotonic_ns();
            void *data = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
            pthread_mutex_lock(&enc->mutex);
            if (data != NULL && pack.u32Len > 0) {
                queue_push_locked(enc, data, pack.u32Len, au_is_keyframe(data, pack.u32Len),
                                  encoded_ns);
            }
            pthread_mutex_unlock(&enc->mutex);
            RK_MPI_VENC_ReleaseStream(enc->venc_chn, &stream);
        }
    }
    return NULL;
}
#endif

static void capture_fill_fmt(struct v4l2_format *fmt, int32_t width, int32_t height, uint32_t fourcc)
{
    memset(fmt, 0, sizeof(*fmt));
    fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt->fmt.pix_mp.width = (uint32_t)width;
    fmt->fmt.pix_mp.height = (uint32_t)height;
    fmt->fmt.pix_mp.pixelformat = fourcc;
    fmt->fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt->fmt.pix_mp.num_planes = 1;
    if (height > 576) {
        fmt->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
        fmt->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
    } else {
        fmt->fmt.pix_mp.colorspace = V4L2_COLORSPACE_SMPTE170M;
        fmt->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
    }
    fmt->fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    fmt->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
}

static uint32_t capture_preferred_fourcc(void)
{
    return rk_rga_present() ? V4L2_PIX_FMT_NV16 : V4L2_PIX_FMT_YUYV;
}

static bool capture_s_fmt(int fd, int32_t width, int32_t height, uint32_t first,
                          struct v4l2_format *fmt)
{
    uint32_t second = first == V4L2_PIX_FMT_NV16 ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_NV16;

    capture_fill_fmt(fmt, width, height, first);
    if (xioctl(fd, VIDIOC_S_FMT, fmt) == 0) {
        return true;
    }
    capture_fill_fmt(fmt, width, height, second);
    return xioctl(fd, VIDIOC_S_FMT, fmt) == 0;
}

#ifndef ONEKVM_HAVE_ROCKIT
static int capture_reqbufs(int fd, uint32_t mem, unsigned count)
{
    struct v4l2_requestbuffers req;

    memset(&req, 0, sizeof(req));
    req.count = count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = mem;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        return -1;
    }
    return (int)req.count;
}

static int capture_qbuf_all(void *source, int fd, uint32_t mem, const int *dma_fds, uint32_t plane_len,
                            unsigned queued, struct rk_cif_hold *hold)
{
    return capture_prime_cif(source, fd, mem, dma_fds, plane_len, queued, hold);
}

static bool capture_timing_supported(int32_t width, int32_t height)
{
    return width >= 320 && height >= 200 && width <= 1920 && height <= 1080 &&
           (width & 1) == 0 && (height & 1) == 0;
}

static bool capture_restart_done(struct rk_encoder *enc, bool result)
{
    kmpp_drain_start(enc);
    return result;
}

/* HDMI login/mode change: CIF size-err or SOURCE_CHANGE, then DQ stops.
 * STREAMOFF without STREAMON again left WebRTC frozen. Re-lock timings and
 * reopen the pipe; reopen VEPU only if WxH actually changed. */
static bool capture_restart_stream(struct rk_encoder *enc, int fd, enum v4l2_buf_type type,
                                   uint32_t mem, int *dma_fds, unsigned *queued,
                                   int32_t *width, int32_t *height, int32_t *fps,
                                   int32_t *encode_fps, uint32_t *fourcc, uint32_t *hor_stride,
                                   uint32_t *enc_fmt, uint32_t *plane_len, unsigned *skip_dq,
                                   struct rk_cif_hold *hold, int32_t *src_w, int32_t *src_h)
{
    struct v4l2_format fmt;
    int32_t q_w = 0;
    int32_t q_h = 0;
    int32_t q_fps = 0;
    bool hdmi = false;
    int tries;
    int32_t old_w = *width;
    int32_t old_h = *height;
    int32_t old_src_w = *src_w;
    int32_t old_src_h = *src_h;

    rk_snapshot_quiesce(enc->bound_source);
    capture_wait_vepu_idle(enc);
    kmpp_drain_stop(enc);
    pthread_mutex_lock(&enc->mutex);
    enc->drain_pause = true;
    queue_flush_locked(enc);
    enc->have_au = false;
    enc->au_done = 0;
    enc->in_flight = 0;
    enc->skip_au = true;
    latency_submit_reset_locked(enc);
    pthread_mutex_unlock(&enc->mutex);
    if (fd >= 0) {
        (void)xioctl(fd, VIDIOC_STREAMOFF, &type);
        /* S_FMT fails with "queue busy" unless REQBUFS has been released. */
        (void)capture_reqbufs(fd, mem, 0);
    }
    /* SOURCE_CHANGE often arrives before TMDS actually drops. STREAMON on
     * the still-cached 1080p lock then sticks CIF in size-err. */
    for (tries = 0; tries < 20 && !enc->stop; tries++) {
        rk_source_timings(enc->bound_source, &q_w, &q_h, &q_fps, &hdmi);
        if (!hdmi || q_w != old_src_w || q_h != old_src_h) {
            break;
        }
        usleep(50000);
    }
    {
        int32_t stable_w = 0;
        int32_t stable_h = 0;
        int32_t stable_fps = 0;
        int stable = 0;

        for (tries = 0; tries < 80 && !enc->stop; tries++) {
            rk_source_timings(enc->bound_source, &q_w, &q_h, &q_fps, &hdmi);
            if (hdmi && capture_timing_supported(q_w, q_h)) {
                if (q_w == stable_w && q_h == stable_h &&
                    (q_fps == 0 || stable_fps == 0 || q_fps == stable_fps)) {
                    if (q_fps > 0) {
                        stable_fps = q_fps;
                    }
                    stable++;
                    if (stable >= RK_STABLE_TIMING_SAMPLES) {
                        break;
                    }
                } else {
                    stable_w = q_w;
                    stable_h = q_h;
                    stable_fps = q_fps;
                    stable = 1;
                }
            } else {
                stable = 0;
                /* This wait can last 8 s: keep the no-signal frame going. */
                if (!hdmi) {
                    ns_tick(enc, old_w, old_h, monotonic_ns());
                }
            }
            usleep(100000);
        }
        ns_park(enc);
        if (stable < RK_STABLE_TIMING_SAMPLES) {
            fprintf(stderr, "rk-mpp: restart wait timings failed hdmi=%d %dx%d@%d\n", hdmi, q_w,
                    q_h, q_fps);
            return capture_restart_done(enc, false);
        }
    }
    if (!hdmi || !capture_timing_supported(q_w, q_h) || enc->stop) {
        return capture_restart_done(enc, false);
    }
    (void)rk_source_lock_detected_timings(enc->bound_source);
    *src_w = q_w;
    *src_h = q_h;
    *width = q_w;
    *height = q_h;
    if (q_fps > 0) {
        *fps = q_fps;
    }
    *encode_fps = onekvm_encode_fps(*fps, *height);
    (void)rk_source_apply_subdev_format(enc->bound_source, *width, *height);
    if (!capture_s_fmt(fd, *width, *height, capture_preferred_fourcc(), &fmt)) {
        fprintf(stderr, "rk-mpp: restart S_FMT failed errno=%d\n", errno);
        return capture_restart_done(enc, false);
    }
    *width = (int32_t)fmt.fmt.pix_mp.width;
    *height = (int32_t)fmt.fmt.pix_mp.height;
    *fourcc = fmt.fmt.pix_mp.pixelformat;
    *hor_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (*hor_stride == 0) {
        *hor_stride = *fourcc == V4L2_PIX_FMT_NV16 ? (uint32_t)*width : (uint32_t)*width * 2u;
    }
    *enc_fmt = kmpp_fmt_from_fourcc(*fourcc);
    (void)rk_source_apply_subdev_format(enc->bound_source, *width, *height);
    /* Reopen VEPU on a size change, after a failed open, from the no-signal
     * channel, and when RGA scaling broke so the channel falls back to the
     * native size. */
    if (*width != old_w || *height != old_h || enc->vcodec_fd < 0 || enc->ns_channel ||
        (enc->scaled && enc->scale == NULL)) {
        usleep(4000);
        kmpp_close_encoder(enc);
        if (kmpp_open_encoder(enc, *width, *height, *encode_fps, *fourcc, *hor_stride, false) != 0) {
            fprintf(stderr, "rk-mpp: restart encoder %dx%d failed errno=%d\n", *width, *height,
                    errno);
            return capture_restart_done(enc, false);
        }
        enc->force_keyframe = true;
    } else {
        pthread_mutex_lock(&enc->mutex);
        enc->force_keyframe = true;
        pthread_mutex_unlock(&enc->mutex);
    }
    {
        int n = capture_reqbufs(fd, mem, *queued > 0 ? *queued : ONEKVM_RK_MPP_BUFFER_COUNT);
        struct v4l2_buffer query;
        struct v4l2_plane plane;
        uint32_t need;

        if (n < 2) {
            fprintf(stderr, "rk-mpp: restart REQBUFS failed errno=%d\n", errno);
            return capture_restart_done(enc, false);
        }
        *queued = (unsigned)n;
        memset(&query, 0, sizeof(query));
        memset(&plane, 0, sizeof(plane));
        query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        query.memory = mem;
        query.m.planes = &plane;
        query.length = 1;
        if (xioctl(fd, VIDIOC_QUERYBUF, &query) != 0 || plane.length == 0) {
            fprintf(stderr, "rk-mpp: restart QUERYBUF failed errno=%d\n", errno);
            return capture_restart_done(enc, false);
        }
        /* CIF rounds the allocation height up independently of S_FMT.
         * Its QUERYBUF length can exceed sizeimage (1080p: 4177920 vs
         * 4147200), especially when returning from a smaller BIOS mode. */
        need = plane.length;
        if (need > *plane_len) {
            if (mem == V4L2_MEMORY_DMABUF) {
                unsigned i;

                kmpp_close_fds(dma_fds, ONEKVM_RK_MPP_BUFFER_COUNT);
                for (i = 0; i < *queued; i++) {
                    int dma_fd = kmpp_alloc_dma_fd(enc, need);

                    if (dma_fd < 0) {
                        fprintf(stderr, "rk-mpp: restart ALLOC_DMA[%u] len=%u errno=%d\n", i,
                                need, errno);
                        return capture_restart_done(enc, false);
                    }
                    dma_fds[i] = dma_fd;
                }
            }
            *plane_len = need;
        }
    }
    if (capture_qbuf_all(enc->bound_source, fd, mem, dma_fds, *plane_len, *queued, hold) != 0) {
        fprintf(stderr, "rk-mpp: restart QBUF failed errno=%d\n", errno);
        return capture_restart_done(enc, false);
    }
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        fprintf(stderr, "rk-mpp: restart STREAMON failed errno=%d\n", errno);
        return capture_restart_done(enc, false);
    }
    *skip_dq = 2;
    fprintf(stderr, "rk-mpp: capture restart %dx%d@%d (was %dx%d)\n", *width, *height, *fps, old_w,
            old_h);
    return capture_restart_done(enc, true);
}
#endif

static void *capture_drain_run(void *arg)
{
    struct rk_encoder *enc = arg;
    const char *device;
    int fd = -1;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_plane planes[ONEKVM_RK_MPP_BUFFER_COUNT];
    unsigned i;
    int32_t width = 1920;
    int32_t height = 1080;
    /* The detected timing size; the bridge may round the format width down. */
    int32_t src_w = 0;
    int32_t src_h = 0;
    int32_t fps = 60;
    int32_t encode_fps = 30;
    uint32_t fourcc = V4L2_PIX_FMT_YUYV;
    uint32_t hor_stride = 0;
#ifndef ONEKVM_HAVE_ROCKIT
    int dma_fds[ONEKVM_RK_MPP_BUFFER_COUNT];
    void *dma_maps[ONEKVM_RK_MPP_BUFFER_COUNT];
    size_t dma_map_lens[ONEKVM_RK_MPP_BUFFER_COUNT];
    bool use_dmabuf = false;
    uint32_t dma_plane_len = 0;
#endif

    device = rk_source_device(enc->bound_source);
    /* Wait for QUERY_DV_TIMINGS. STREAMON at CIF's 640x480 default while HDMI
     * is 1080p programs 1-lane DPHY, size-err, and sticks on placeholder bars. */
    {
        int tries;
        bool hdmi = false;
        int32_t q_w = 0;
        int32_t q_h = 0;
        int32_t q_fps = 0;

        for (tries = 0; tries < 150 && !enc->stop; tries++) {
            rk_source_timings(enc->bound_source, &q_w, &q_h, &q_fps, &hdmi);
            if (hdmi && capture_timing_supported(q_w, q_h)) {
                width = q_w;
                height = q_h;
                src_w = q_w;
                src_h = q_h;
                if (q_fps > 0) {
                    fps = q_fps;
                }
                (void)rk_source_lock_detected_timings(enc->bound_source);
                break;
            }
#ifndef ONEKVM_HAVE_ROCKIT
            /* Show the no-signal frame while the source has none. */
            if (!hdmi) {
                ns_tick(enc, width, height, monotonic_ns());
            }
#endif
            usleep(100000);
        }
    }
    if (width <= 0 || height <= 0) {
        width = 1920;
        height = 1080;
    }

#ifdef ONEKVM_HAVE_ROCKIT
    if (!enc->sys_inited) {
        if (RK_MPI_SYS_Init() == RK_SUCCESS) {
            enc->sys_inited = true;
        }
    }
    if (enc->sys_inited && venc_create(enc, width, height, fps) == RK_SUCCESS) {
        enc->venc_started = true;
        enc->stop = false;
        if (pthread_create(&enc->venc_thread, NULL, venc_reader, enc) != 0) {
            enc->venc_started = false;
        }
    }
#endif

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
#ifndef ONEKVM_HAVE_ROCKIT
        ns_until_stop(enc, width, height);
#endif
        return NULL;
    }
    enc->capture_fd = fd;
    (void)rk_source_apply_subdev_format(enc->bound_source, width, height);
    if (!capture_s_fmt(fd, width, height, capture_preferred_fourcc(), &fmt)) {
        close(fd);
        enc->capture_fd = -1;
#ifndef ONEKVM_HAVE_ROCKIT
        ns_until_stop(enc, width, height);
#endif
        return NULL;
    }
    if ((int32_t)fmt.fmt.pix_mp.width != width ||
        (int32_t)fmt.fmt.pix_mp.height != height) {
        (void)rk_source_apply_subdev_format(enc->bound_source, width, height);
        if (!capture_s_fmt(fd, width, height, capture_preferred_fourcc(), &fmt) ||
            (int32_t)fmt.fmt.pix_mp.width != width ||
            (int32_t)fmt.fmt.pix_mp.height != height) {
            fprintf(stderr, "rk-mpp: S_FMT stayed %ux%u, wanted %dx%d\n",
                    fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, width, height);
        }
    }
    width = (int32_t)fmt.fmt.pix_mp.width;
    height = (int32_t)fmt.fmt.pix_mp.height;
    fourcc = fmt.fmt.pix_mp.pixelformat;
    hor_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (hor_stride == 0) {
        hor_stride = fourcc == V4L2_PIX_FMT_NV16 ? (uint32_t)width : (uint32_t)width * 2u;
    }
    (void)rk_source_apply_subdev_format(enc->bound_source, width, height);
    memset(&req, 0, sizeof(req));
    req.count = ONEKVM_RK_MPP_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
#ifdef ONEKVM_HAVE_ROCKIT
    req.memory = V4L2_MEMORY_DMABUF;
#else
    for (i = 0; i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
        dma_fds[i] = -1;
    }
    encode_fps = onekvm_encode_fps(fps, height);
    ns_close(enc);
    if (kmpp_open_encoder(enc, width, height, encode_fps, fourcc, hor_stride, false) != 0) {
        fprintf(stderr, "rk-mpp: kmpp_open_encoder failed errno=%d\n", errno);
        req.memory = V4L2_MEMORY_MMAP;
    } else {
        req.memory = V4L2_MEMORY_DMABUF;
    }
#endif
    {
        int req_ok = xioctl(fd, VIDIOC_REQBUFS, &req);
#ifndef ONEKVM_HAVE_ROCKIT
        if (req_ok < 0 && req.memory == V4L2_MEMORY_DMABUF) {
            memset(&req, 0, sizeof(req));
            req.count = ONEKVM_RK_MPP_BUFFER_COUNT;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            req.memory = V4L2_MEMORY_MMAP;
            req_ok = xioctl(fd, VIDIOC_REQBUFS, &req);
        }
        use_dmabuf = req.memory == V4L2_MEMORY_DMABUF;
#endif
        if (req_ok < 0) {
#ifndef ONEKVM_HAVE_ROCKIT
            kmpp_close_encoder(enc);
            kmpp_close_fds(dma_fds, ONEKVM_RK_MPP_BUFFER_COUNT);
#endif
            close(fd);
            enc->capture_fd = -1;
#ifndef ONEKVM_HAVE_ROCKIT
            ns_until_stop(enc, width, height);
#endif
            return NULL;
        }
    }

#ifdef ONEKVM_HAVE_ROCKIT
    {
        MB_POOL pool = MB_INVALID_POOLID;
        MB_POOL_CONFIG_S cfg;
        MB_BLK blks[ONEKVM_RK_MPP_BUFFER_COUNT];

        memset(&cfg, 0, sizeof(cfg));
        cfg.u64MBSize = 1920ull * 1080ull * 3u;
        cfg.u32MBCnt = ONEKVM_RK_MPP_BUFFER_COUNT;
        cfg.enAllocType = MB_ALLOC_TYPE_DMA;
        cfg.bPreAlloc = RK_TRUE;
        pool = RK_MPI_MB_CreatePool(&cfg);
        if (pool == MB_INVALID_POOLID) {
            close(fd);
            enc->capture_fd = -1;
            return NULL;
        }
        memset(blks, 0, sizeof(blks));
        for (i = 0; i < req.count && i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
            memset(&buf, 0, sizeof(buf));
            memset(&planes[i], 0, sizeof(planes[i]));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory = V4L2_MEMORY_DMABUF;
            buf.index = i;
            buf.m.planes = &planes[i];
            buf.length = 1;
            if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                break;
            }
            blks[i] = RK_MPI_MB_GetMB(pool, planes[i].length, RK_TRUE);
            if (blks[i] == RK_NULL) {
                break;
            }
            planes[i].m.fd = RK_MPI_MB_Handle2Fd(blks[i]);
            if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                break;
            }
        }
        if (xioctl(fd, VIDIOC_STREAMON, &type) == 0) {
            VIDEO_FRAME_INFO_S frame;
            memset(&frame, 0, sizeof(frame));
            frame.stVFrame.u32Width = (RK_U32)width;
            frame.stVFrame.u32Height = (RK_U32)height;
            frame.stVFrame.u32VirWidth = ((uint32_t)width + 15u) & ~15u;
            frame.stVFrame.u32VirHeight = ((uint32_t)height + 15u) & ~15u;
            frame.stVFrame.enPixelFormat = RK_FMT_YUV422_YUYV;
            frame.stVFrame.enField = VIDEO_FIELD_FRAME;
            while (!enc->stop) {
                struct pollfd pfd = {.fd = fd, .events = POLLIN};
                if (poll(&pfd, 1, 200) <= 0) {
                    continue;
                }
                memset(&buf, 0, sizeof(buf));
                memset(&planes[0], 0, sizeof(planes[0]));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                buf.memory = V4L2_MEMORY_DMABUF;
                buf.m.planes = &planes[0];
                buf.length = 1;
                if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
                    break;
                }
                if (enc->force_keyframe && enc->venc_started) {
                    RK_MPI_VENC_RequestIDR(enc->venc_chn, RK_TRUE);
                    enc->force_keyframe = false;
                }
                frame.stVFrame.pMbBlk = RK_MPI_MMZ_Fd2Handle(planes[0].m.fd);
                if (enc->venc_started && frame.stVFrame.pMbBlk != RK_NULL) {
                    (void)RK_MPI_VENC_SendFrame(enc->venc_chn, &frame, 2000);
                }
                (void)xioctl(fd, VIDIOC_QBUF, &buf);
            }
            xioctl(fd, VIDIOC_STREAMOFF, &type);
        }
        for (i = 0; i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
            if (blks[i] != RK_NULL) {
                RK_MPI_MB_ReleaseMB(blks[i]);
            }
        }
        RK_MPI_MB_DestroyPool(pool);
    }
#else
    {
        uint64_t last_signal_ns = 0;
        uint64_t no_signal_since_ns = 0;
        uint64_t last_dbg_ns = 0;
        bool streaming;
        bool hdmi = false;
        unsigned skip_dq = 2;
        uint32_t enc_fmt = kmpp_fmt_from_fourcc(fourcc);
        uint32_t min_bytes;
        uint32_t sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        unsigned queued = 0;
        bool dmabuf_failed = false;
        uint32_t mem = use_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        struct rk_cif_hold hold;

        cif_hold_reset(&hold);
        hold.source = enc->bound_source;
        rk_source_timings(enc->bound_source, NULL, NULL, NULL, &hdmi);
        display_unmap(dma_maps, dma_map_lens, ONEKVM_RK_MPP_BUFFER_COUNT);
        memset(dma_maps, 0, sizeof(dma_maps));
        memset(dma_map_lens, 0, sizeof(dma_map_lens));
    queue_bufs:
        queued = 0;
        dmabuf_failed = false;
        cif_hold_reset(&hold);
        hold.source = enc->bound_source;
        display_unmap(dma_maps, dma_map_lens, ONEKVM_RK_MPP_BUFFER_COUNT);
        for (i = 0; i < req.count && i < ONEKVM_RK_MPP_BUFFER_COUNT; i++) {
            memset(&buf, 0, sizeof(buf));
            memset(&planes[i], 0, sizeof(planes[i]));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory = mem;
            buf.index = i;
            buf.m.planes = &planes[i];
            buf.length = 1;
            if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                fprintf(stderr, "rk-mpp: QUERYBUF[%u] errno=%d\n", i, errno);
                break;
            }
            /* MPLANE: v4l2_buffer.length is nplanes, not bytes. QUERYBUF may
             * overwrite it with sizeimage; QBUF then copy_from_user EFAULT. */
            buf.length = 1;
            buf.m.planes = &planes[i];
            if (i == 0) {
                dma_plane_len = planes[i].length;
                fprintf(stderr,
                        "rk-mpp: QUERYBUF length=%u sizeimage=%u stride=%u height=%d\n",
                        dma_plane_len, sizeimage, hor_stride, height);
            }
            if (use_dmabuf) {
                int dma_fd = kmpp_alloc_dma_fd(enc, planes[i].length);
                if (dma_fd < 0) {
                    fprintf(stderr, "rk-mpp: ALLOC_DMA[%u] len=%u errno=%d\n", i,
                            planes[i].length, errno);
                    if (queued >= 3) {
                        break;
                    }
                    dmabuf_failed = true;
                    break;
                }
                dma_fds[i] = dma_fd;
                planes[i].m.fd = dma_fd;
                display_map_fd(dma_maps, dma_map_lens, i, dma_fd, planes[i].length);
            } else {
                struct v4l2_exportbuffer exp;

                memset(&exp, 0, sizeof(exp));
                exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                exp.index = i;
                exp.plane = 0;
                exp.flags = O_RDWR | O_CLOEXEC;
                if (xioctl(fd, VIDIOC_EXPBUF, &exp) == 0) {
                    dma_fds[i] = exp.fd;
                    display_map_fd(dma_maps, dma_map_lens, i, exp.fd, planes[i].length);
                } else {
                    fprintf(stderr, "rk-mpp: EXPBUF[%u] errno=%d\n", i, errno);
                }
            }
            if (hold.cif_n < RK_CIF_KEEP) {
                if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                    fprintf(stderr, "rk-mpp: QBUF[%u] errno=%d mem=%u length=%u fd=%d\n", i, errno,
                            mem, planes[i].length, planes[i].m.fd);
                    break;
                }
                note_slot_qbuf(hold.qbuf_ns, i);
                hold.cif_n++;
            } else {
                cif_spare_push(&hold, i);
            }
            queued++;
        }
        if (dmabuf_failed && use_dmabuf) {
            struct v4l2_requestbuffers free_req;

            display_unmap(dma_maps, dma_map_lens, ONEKVM_RK_MPP_BUFFER_COUNT);
            kmpp_close_fds(dma_fds, ONEKVM_RK_MPP_BUFFER_COUNT);
            memset(&free_req, 0, sizeof(free_req));
            free_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            (void)xioctl(fd, VIDIOC_REQBUFS, &free_req);
            memset(&req, 0, sizeof(req));
            req.count = ONEKVM_RK_MPP_BUFFER_COUNT;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            req.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &req) == 0) {
                use_dmabuf = false;
                mem = V4L2_MEMORY_MMAP;
                fprintf(stderr, "rk-mpp: VEPU dma alloc failed, fallback MMAP\n");
                goto queue_bufs;
            }
        }
        if (enc->vcodec_fd >= 0 &&
            pthread_create(&enc->drain_thread, NULL, kmpp_drain_thread, enc) == 0) {
            enc->drain_started = true;
        }
        streaming = hold.cif_n > 0 && xioctl(fd, VIDIOC_STREAMON, &type) == 0;
        fprintf(stderr,
                "rk-mpp: capture %dx%d@%d enc_fps=%d fourcc=%08x stride=%u vcodec=%d stream=%d "
                "dma0=%d dmabuf=%d queued=%u cif=%u spare=%u plane=%u\n",
                width, height, fps, encode_fps, fourcc, hor_stride, enc->vcodec_fd, streaming,
                dma_fds[0], use_dmabuf, queued, hold.cif_n, hold.spare_n, dma_plane_len);
        {
            unsigned poll_to = 0;
            unsigned dq = 0;
            unsigned infrm_ok = 0;
            unsigned infrm_fail = 0;
            unsigned dq_err = 0;
            int subdev_fd = rk_source_subdev_fd(enc->bound_source);
            uint64_t last_dq_ns = streaming ? monotonic_ns() : 0;
            uint64_t last_restart_ns = 0;
            uint64_t snapshot_hold_ns = 0;
            unsigned idr_tick = 0;

            if (subdev_fd >= 0) {
                struct v4l2_event_subscription sub;

                memset(&sub, 0, sizeof(sub));
                sub.type = V4L2_EVENT_SOURCE_CHANGE;
                (void)xioctl(subdev_fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
            }

            while (!enc->stop) {
                struct pollfd pfds[3];
                uint64_t now = monotonic_ns();
                int pr;
                bool live;
                bool need_restart = false;
                int32_t q_w = width;
                int32_t q_h = height;
                int32_t q_fps = fps;

                if (rk_snapshot_source_busy(enc->bound_source)) {
                    snapshot_hold_ns = now;
                }

                capture_reclaim_done(enc, fd, mem, dma_fds, dma_plane_len, &hold);
                pthread_mutex_lock(&enc->mutex);
                live = enc->have_au;
                pthread_mutex_unlock(&enc->mutex);
                if (now - last_signal_ns >= 200000000ull) {
                    rk_source_timings(enc->bound_source, &q_w, &q_h, &q_fps, &hdmi);
                    last_signal_ns = now;
                    if (hdmi) {
                        no_signal_since_ns = 0;
                    } else if (no_signal_since_ns == 0) {
                        no_signal_since_ns = now;
                    }
                    if (hdmi && capture_timing_supported(q_w, q_h) &&
                        (q_w != src_w || q_h != src_h ||
                         (q_fps > 0 && (q_fps > fps + 1 || q_fps + 1 < fps)))) {
                        need_restart = true;
                    }
                }
                /* Capture is stopped: VEPU encodes the no-signal frame. */
                if (!streaming) {
                    ns_tick(enc, width, height, now);
                }
                if (!streaming && hdmi && capture_timing_supported(q_w, q_h)) {
                    need_restart = true;
                }
                if (streaming && enc->scaled && enc->scale == NULL) {
                    need_restart = true;
                }
                if (streaming && no_signal_since_ns != 0 &&
                    now - no_signal_since_ns >= RK_SIGNAL_LOSS_NS) {
                    capture_wait_vepu_idle(enc);
                    (void)xioctl(fd, VIDIOC_STREAMOFF, &type);
                    streaming = false;
                    last_dq_ns = 0;
                    capture_reset_inflight(enc, &hold);
                }
                if (streaming && last_dq_ns != 0 && now - last_dq_ns >= RK_STALL_NS &&
                    now - snapshot_hold_ns >= RK_STALL_NS) {
                    if (!hdmi) {
                        capture_wait_vepu_idle(enc);
                        (void)xioctl(fd, VIDIOC_STREAMOFF, &type);
                        streaming = false;
                        last_dq_ns = 0;
                        capture_reset_inflight(enc, &hold);
                    } else {
                        need_restart = true;
                    }
                }
                if (need_restart && now - last_restart_ns >= RK_RESTART_MIN_NS) {
                    streaming = capture_restart_stream(
                        enc, fd, type, mem, dma_fds, &queued, &width, &height, &fps, &encode_fps,
                        &fourcc, &hor_stride, &enc_fmt, &dma_plane_len, &skip_dq, &hold, &src_w, &src_h);
                    last_restart_ns = monotonic_ns();
                    last_dq_ns = streaming ? last_restart_ns : 0;
                    capture_reset_inflight(enc, &hold);
                    if (streaming) {
                        display_remap_all(dma_maps, dma_map_lens, dma_fds, queued, dma_plane_len);
                    }
                    continue;
                }
                if (!streaming) {
                    usleep(20000);
                    continue;
                }
                pfds[0].fd = fd;
                pfds[0].events = POLLIN;
                pfds[0].revents = 0;
                pfds[1].fd = subdev_fd;
                pfds[1].events = POLLPRI;
                pfds[1].revents = 0;
                pfds[2].fd = enc->capture_wake;
                pfds[2].events = POLLIN;
                pfds[2].revents = 0;
                pr = poll(pfds, 3, RK_LIVE_POLL_MS);
                if (pfds[2].revents & POLLIN) {
                    clear_wake_fd(enc->capture_wake);
                    if (enc->stop) {
                        break;
                    }
                    capture_reclaim_done(enc, fd, mem, dma_fds, dma_plane_len, &hold);
                }
                if (pr > 0 && (pfds[1].revents & POLLPRI) != 0) {
                    struct v4l2_event ev;
                    bool src_change = false;
                    int32_t ev_w = width;
                    int32_t ev_h = height;
                    int32_t ev_fps = fps;
                    bool ev_hdmi = hdmi;

                    /* DQEVENT blocks when the queue is empty. Draining with a
                     * blocking ioctl parked the capture thread so HPD never
                     * restarted, and unbind then SEGV'd the waiter. */
                    for (;;) {
                        struct pollfd evp = {.fd = subdev_fd, .events = POLLPRI, .revents = 0};

                        if (poll(&evp, 1, 0) <= 0 || (evp.revents & POLLPRI) == 0) {
                            break;
                        }
                        memset(&ev, 0, sizeof(ev));
                        if (xioctl(subdev_fd, VIDIOC_DQEVENT, &ev) < 0) {
                            break;
                        }
                        if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                            src_change = true;
                        }
                    }
                    if (src_change) {
                        rk_source_timings(enc->bound_source, &ev_w, &ev_h, &ev_fps, &ev_hdmi);
                        if (streaming && last_dq_ns != 0 && now - last_dq_ns < RK_STALL_NS &&
                            ev_hdmi && ev_w == src_w && ev_h == src_h) {
                            continue;
                        }
                        if (now - last_restart_ns >= RK_RESTART_MIN_NS) {
                            streaming = capture_restart_stream(
                                enc, fd, type, mem, dma_fds, &queued, &width, &height, &fps,
                                &encode_fps, &fourcc, &hor_stride, &enc_fmt, &dma_plane_len,
                                &skip_dq, &hold, &src_w, &src_h);
                            last_restart_ns = monotonic_ns();
                            last_dq_ns = streaming ? last_restart_ns : 0;
                            capture_reset_inflight(enc, &hold);
                            if (streaming) {
                                display_remap_all(dma_maps, dma_map_lens, dma_fds, queued,
                                                  dma_plane_len);
                            }
                        }
                    }
                    continue;
                }
                if (pr <= 0) {
                    poll_to++;
                    if (!live && now - last_dbg_ns >= 10000000000ull) {
                        fprintf(stderr,
                                "rk-mpp: wait poll_to=%u dq=%u infrm_ok=%u infrm_fail=%u "
                                "dq_err=%u live=%d hdmi=%d\n",
                                poll_to, dq, infrm_ok, infrm_fail, dq_err, live, hdmi);
                        last_dbg_ns = now;
                    }
                    continue;
                }
                if (!(pfds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
                    continue;
                }
                memset(&buf, 0, sizeof(buf));
                memset(&planes[0], 0, sizeof(planes[0]));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                buf.memory = mem;
                buf.m.planes = &planes[0];
                buf.length = 1;
                if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
                    dq_err++;
                    if (now - last_restart_ns >= RK_RESTART_MIN_NS) {
                        streaming = capture_restart_stream(
                            enc, fd, type, mem, dma_fds, &queued, &width, &height, &fps,
                            &encode_fps, &fourcc, &hor_stride, &enc_fmt, &dma_plane_len,
                            &skip_dq, &hold, &src_w, &src_h);
                        last_restart_ns = monotonic_ns();
                        last_dq_ns = streaming ? last_restart_ns : 0;
                        capture_reset_inflight(enc, &hold);
                        if (streaming) {
                            display_remap_all(dma_maps, dma_map_lens, dma_fds, queued,
                                              dma_plane_len);
                        }
                    }
                    continue;
                }
                dq++;
                if (hold.cif_n > 0) {
                    hold.cif_n--;
                }
                note_cif_capture(enc, &buf,
                                 buf.index < ONEKVM_RK_MPP_BUFFER_COUNT
                                     ? hold.qbuf_ns[buf.index]
                                     : 0);
                /* Start the next CIF fill before IN_FRM. KEEP=1: this is the
                 * only driver slot; waiting until AU would add encode time. */
                cif_feed(fd, mem, dma_fds, dma_plane_len, &hold);
                if (skip_dq > 0) {
                    skip_dq--;
                    cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                    continue;
                }
                min_bytes = hor_stride * (uint32_t)height;
                if (min_bytes == 0) {
                    min_bytes = (uint32_t)width * (uint32_t)height * 2u;
                }
                if (planes[0].bytesused < (min_bytes * 3u) / 4u) {
                    cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                    continue;
                }
                /* Encode the newest ready CIF frame only if a replacement can
                 * be QBUF'd first. Extra-DQ with spare==0 leaves one slot in
                 * the driver; ping-pong then misses SOF and locks at 30 fps. */
                for (;;) {
                    struct pollfd extra_p = {.fd = fd, .events = POLLIN, .revents = 0};
                    struct v4l2_buffer extra;
                    struct v4l2_plane epl;

                    if (hold.spare_n == 0) {
                        break;
                    }
                    if (poll(&extra_p, 1, 0) <= 0 || (extra_p.revents & POLLIN) == 0) {
                        break;
                    }
                    memset(&extra, 0, sizeof(extra));
                    memset(&epl, 0, sizeof(epl));
                    extra.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                    extra.memory = mem;
                    extra.m.planes = &epl;
                    extra.length = 1;
                    if (xioctl(fd, VIDIOC_DQBUF, &extra) < 0) {
                        break;
                    }
                    dq++;
                    if (hold.cif_n > 0) {
                        hold.cif_n--;
                    }
                    cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                    planes[0] = epl;
                    buf = extra;
                    buf.m.planes = &planes[0];
                    note_cif_capture(enc, &buf,
                                     buf.index < ONEKVM_RK_MPP_BUFFER_COUNT
                                         ? hold.qbuf_ns[buf.index]
                                         : 0);
                    cif_feed(fd, mem, dma_fds, dma_plane_len, &hold);
                }
                now = monotonic_ns();
                last_dq_ns = now;
                if (buf.index < ONEKVM_RK_MPP_BUFFER_COUNT &&
                    (dma_maps[buf.index] != NULL || dma_fds[buf.index] >= 0)) {
                    rk_snapshot_offer(enc->bound_source, dma_fds[buf.index], width, height,
                                        fourcc, hor_stride, dma_plane_len, now);
                    rk_source_publish_display(enc->bound_source, dma_maps[buf.index],
                                              dma_fds[buf.index], fourcc, width, height, hor_stride,
                                              now);
                }
                /* Do not skip on 1/encode_fps. CIF 1080p60 arrives ~16.67 ms
                 * apart; jitter makes `now - last < 1e9/60` drop every other
                 * frame and lock VEPU at ~30. Let HDMI/CIF pace IN_FRM.
                 * Display-only keeps this drain for RGA but does not call
                 * read_packet; pause VEPU until a reader shows up. */
                if (enc->vcodec_fd >= 0 && buf.index < ONEKVM_RK_MPP_BUFFER_COUNT &&
                    dma_fds[buf.index] >= 0) {
                    struct kmpp_frame_infos info;
                    bool want_vepu;

                    pthread_mutex_lock(&enc->mutex);
                    want_vepu = reader_active(enc->last_read_ns, now);
                    if (!want_vepu) {
                        if (!enc->vepu_paused) {
                            queue_flush_locked(enc);
                            enc->vepu_paused = true;
                        }
                        pthread_mutex_unlock(&enc->mutex);
                        cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                    } else {
                    if (enc->vepu_paused) {
                        enc->vepu_paused = false;
                        enc->force_keyframe = true;
                    }
                    if (enc->force_keyframe) {
                        (void)kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_CONTROL,
                                         KMPP_ENC_SET_IDR_FRAME, NULL, 0);
                        enc->force_keyframe = false;
                    }
                    pthread_mutex_unlock(&enc->mutex);
                    memset(&info, 0, sizeof(info));
                    info.width = (uint32_t)width;
                    info.height = (uint32_t)height;
                    info.hor_stride = hor_stride;
                    info.ver_stride = rk_ver_stride(fourcc, (uint32_t)height);
                    info.hor_stride_pixel = (uint32_t)width;
                    info.fmt = enc_fmt;
                    info.fd = (uint32_t)dma_fds[buf.index];
                    /* kmpp/RC treat pts as microseconds. Nanoseconds make the
                     * encoder think frames are seconds apart and look frozen. */
                    info.pts = now / 1000ull;
                    info.jpeg_chan_id = -1;
                    capture_reclaim_done(enc, fd, mem, dma_fds, dma_plane_len, &hold);
                    if (hold.pend_n >= RK_MAX_INFLIGHT) {
                        capture_wait_au(enc, hold.released);
                        capture_reclaim_done(enc, fd, mem, dma_fds, dma_plane_len, &hold);
                    }
                    if (hold.pend_n >= RK_MAX_INFLIGHT) {
                        cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                    } else {
                        if (enc->scaled) {
                            /* Without RGA output this channel has nothing to
                             * encode: drop the frame, never submit the CIF
                             * buffer at the wrong size. */
                            if (enc->scale == NULL ||
                                rk_rga_dma_run(enc->scale, dma_fds[buf.index], dma_plane_len,
                                               enc->scale_fd, 100) != 0) {
                                cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                                if (enc->scale != NULL) {
                                    scale_recover(enc, fourcc, width, height, hor_stride);
                                }
                                continue;
                            }
                            enc->scale_failures = 0;
                            info.width = (uint32_t)enc->active_width;
                            info.height = (uint32_t)enc->active_height;
                            info.hor_stride = info.width;
                            info.ver_stride = info.height;
                            info.hor_stride_pixel = info.width;
                            info.fmt = KMPP_FMT_YUV420SP;
                            info.fd = (uint32_t)enc->scale_fd;
                        }
                        pthread_mutex_lock(&enc->mutex);
                        enc->skip_au = false;
                        enc->in_flight++;
                        latency_submit_push_locked(enc, monotonic_ns());
                        pthread_mutex_unlock(&enc->mutex);
                        if (kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_IN_FRM_RDY, 0, &info,
                                       (uint32_t)sizeof(info)) == 0) {
                            infrm_ok++;
                            hold.pending[(hold.pend_head + hold.pend_n) %
                                         ONEKVM_RK_MPP_BUFFER_COUNT] = buf.index;
                            hold.pend_n++;
                            idr_tick++;
                            if (RK_IDR_EVERY > 0 && idr_tick >= RK_IDR_EVERY) {
                                (void)kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_CONTROL,
                                                 KMPP_ENC_SET_IDR_FRAME, NULL, 0);
                                idr_tick = 0;
                            }
                        } else {
                            pthread_mutex_lock(&enc->mutex);
                            if (enc->in_flight > 0) {
                                enc->in_flight--;
                            }
                            latency_submit_pop_tail_locked(enc);
                            pthread_mutex_unlock(&enc->mutex);
                            infrm_fail++;
                            if (now - last_dbg_ns >= 2000000000ull) {
                                fprintf(stderr,
                                        "rk-mpp: IN_FRM_RDY fd=%u errno=%d live=%d hdmi=%d\n",
                                        info.fd, errno, live, hdmi);
                                last_dbg_ns = now;
                            }
                            cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                        }
                    }
                    }
                } else {
                    cif_recycle(fd, mem, dma_fds, dma_plane_len, &hold, buf.index);
                }
                if (now - last_dbg_ns >= 2000000000ull) {
                    unsigned done;
                    unsigned qdrop;
                    unsigned slab_waits;

                    pthread_mutex_lock(&enc->mutex);
                    done = enc->au_done;
                    qdrop = enc->qdrop;
                    slab_waits = enc->slab_waits;
                    pthread_mutex_unlock(&enc->mutex);
                    fprintf(stderr,
                            "rk-mpp: dq=%u infrm_ok=%u infrm_fail=%u dq_err=%u "
                            "live=%d hdmi=%d pend=%u cif=%u spare=%u done=%u rel=%u "
                            "qdrop=%u slabwait=%u\n",
                            dq, infrm_ok, infrm_fail, dq_err, live, hdmi, hold.pend_n,
                            hold.cif_n, hold.spare_n, done, hold.released, qdrop, slab_waits);
                    last_dbg_ns = now;
                }
            }
        }
        rk_snapshot_quiesce(enc->bound_source);
        if (streaming) {
            xioctl(fd, VIDIOC_STREAMOFF, &type);
        }
        if (enc->drain_started) {
            enc->stop = true;
            wake_fd(enc->drain_wake);
            pthread_join(enc->drain_thread, NULL);
            enc->drain_started = false;
        }
        kmpp_close_encoder(enc);
        display_unmap(dma_maps, dma_map_lens, ONEKVM_RK_MPP_BUFFER_COUNT);
        rk_source_release_display(enc->bound_source);
        kmpp_close_fds(dma_fds, ONEKVM_RK_MPP_BUFFER_COUNT);
    }
#endif
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_REQBUFS, &req);
    close(fd);
    enc->capture_fd = -1;
    return NULL;
}

static void *capture_drain(void *arg)
{
    struct rk_encoder *enc = arg;

    (void)capture_drain_run(enc);
    pthread_mutex_lock(&enc->mutex);
    atomic_store(&enc->capture_exited, true);
    pthread_cond_broadcast(&enc->ready);
    pthread_mutex_unlock(&enc->mutex);
    return NULL;
}

static void stop_bound_locked(struct rk_encoder *enc)
{
    if (!enc->capture_started) {
        return;
    }
    enc->stop = true;
    pthread_cond_broadcast(&enc->ready);
#ifndef ONEKVM_HAVE_ROCKIT
    wake_fd(enc->capture_wake);
    wake_fd(enc->drain_wake);
#endif
    pthread_mutex_unlock(&enc->mutex);
    pthread_join(enc->capture_thread, NULL);
    pthread_mutex_lock(&enc->mutex);
    enc->capture_started = false;
    atomic_store(&enc->capture_exited, false);
#ifdef ONEKVM_HAVE_ROCKIT
    if (enc->venc_started) {
        pthread_join(enc->venc_thread, NULL);
        RK_MPI_VENC_StopRecvFrame(enc->venc_chn);
        RK_MPI_VENC_DestroyChn(enc->venc_chn);
        enc->venc_started = false;
    }
#endif
    rk_source_capture_release(enc->bound_source, enc);
    enc->bound_source = NULL;
    enc->stop = false;
    enc->last_read_ns = 0;
    enc->vepu_paused = false;
    queue_clear_locked(enc);
    enc->await_key = false;
    enc->qdrop = 0;
    enc->slab_waits = 0;
}

int32_t rk_encoder_create(const struct onekvm_video_encoder_config_v1 *config, void **out,
                          char *error, uint32_t error_capacity)
{
    struct rk_encoder *enc;

    if (out != NULL) {
        *out = NULL;
    }
    if (config == NULL || out == NULL ||
        config->struct_size < sizeof(struct onekvm_video_encoder_config_v1) ||
        !onekvm_codec_supported(config->codec)) {
        onekvm_set_error(error, error_capacity, "invalid video encoder config");
        return -1;
    }
    enc = calloc(1, sizeof(*enc));
    if (enc == NULL) {
        onekvm_set_error(error, error_capacity, "allocate encoder: out of memory");
        return -1;
    }
    pthread_mutex_init(&enc->mutex, NULL);
    {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&enc->ready, &attr);
        pthread_condattr_destroy(&attr);
    }
    atomic_init(&enc->stop, false);
    atomic_init(&enc->capture_exited, false);
    atomic_init(&enc->color_info, 0u);
    atomic_init(&enc->stream_range, ONEKVM_VIDEO_COLOR_RANGE_UNSPECIFIED);
    enc->config = *config;
    enc->config.codec = onekvm_resolved_codec(config->codec);
    enc->capture_fd = -1;
    enc->vcodec_fd = -1;
#ifndef ONEKVM_HAVE_ROCKIT
    enc->ns_fd = -1;
    atomic_init(&enc->drain_stop, false);
    enc->capture_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    enc->drain_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (enc->capture_wake < 0 || enc->drain_wake < 0) {
        if (enc->capture_wake >= 0) {
            close(enc->capture_wake);
        }
        if (enc->drain_wake >= 0) {
            close(enc->drain_wake);
        }
        pthread_cond_destroy(&enc->ready);
        pthread_mutex_destroy(&enc->mutex);
        free(enc);
        onekvm_set_error(error, error_capacity, "allocate encoder wake fds: %s", strerror(errno));
        return -1;
    }
#endif
    enc->borrowed_slab = -1;
#ifndef ONEKVM_HAVE_ROCKIT
    enc->scale_fd = -1;
#endif
    {
        unsigned qi;

        for (qi = 0; qi < RK_PACKET_QUEUE; qi++) {
            enc->queue[qi].slab = -1;
        }
    }
#ifdef ONEKVM_HAVE_ROCKIT
    enc->venc_chn = 0;
#endif
    pthread_mutex_lock(&g_resource_lock);
    g_active_h26x++;
    pthread_mutex_unlock(&g_resource_lock);
    *out = enc;
    return 0;
}

int32_t rk_encoder_reset(void *opaque, const struct onekvm_video_encoder_config_v1 *config,
                         char *error, uint32_t error_capacity)
{
    struct rk_encoder *enc = opaque;

    if (enc == NULL || config == NULL ||
        config->struct_size < sizeof(*config) || !onekvm_codec_supported(config->codec)) {
        onekvm_set_error(error, error_capacity, "invalid video encoder config");
        return -1;
    }
    pthread_mutex_lock(&enc->mutex);
#ifndef ONEKVM_HAVE_ROCKIT
    if (enc->config.codec == onekvm_resolved_codec(config->codec) &&
        enc->config.fps == config->fps && enc->config.gop == config->gop) {
        if (enc->vcodec_fd >= 0) {
            struct kmpp_onekvm_rate rate = {
                .bps = (uint32_t)onekvm_target_bitrate_kbps(enc->active_width,
                            enc->active_height, config->quality_factor,
                            config->bitrate_kbps) * 1000u,
                .initial_qp = config->initial_qp,
                .min_qp = config->min_qp,
                .max_qp = config->max_qp,
            };
            if (kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_ONEKVM_RATE, 0,
                           &rate, sizeof(rate)) != 0) {
                onekvm_set_error(error, error_capacity, "runtime rate update: %s", strerror(errno));
                pthread_mutex_unlock(&enc->mutex);
                return -1;
            }
            fprintf(stderr, "rk-mpp: rate update bps=%u qp=%d/%d/%d\n", rate.bps,
                    rate.initial_qp, rate.min_qp, rate.max_qp);
        }
        enc->config = *config;
        enc->config.codec = onekvm_resolved_codec(config->codec);
        pthread_mutex_unlock(&enc->mutex);
        return 0;
    }
#endif
    if (enc->capture_started) {
        stop_bound_locked(enc);
    }
    enc->config = *config;
    enc->config.codec = onekvm_resolved_codec(config->codec);
    pthread_mutex_unlock(&enc->mutex);
    return 0;
}

int32_t rk_encoder_encode(void *opaque, const struct onekvm_video_frame_v1 *frame,
                         struct onekvm_video_packet_v1 *packet, char *error,
                         uint32_t error_capacity)
{
    (void)opaque;
    (void)frame;
    (void)packet;
    onekvm_set_error(error, error_capacity, "rk-mpp uses bound encode, not encoder_encode");
    return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
}

int32_t rk_encoder_set_quality(void *opaque, double quality_factor, char *error,
                              uint32_t error_capacity)
{
    struct rk_encoder *enc = opaque;
    if (enc == NULL) {
        return -1;
    }
    pthread_mutex_lock(&enc->mutex);
    struct onekvm_video_encoder_config_v1 config = enc->config;
    config.quality_factor = quality_factor;
    pthread_mutex_unlock(&enc->mutex);
    return rk_encoder_reset(enc, &config, error, error_capacity);
}

int32_t rk_encoder_request_keyframe(void *opaque, char *error, uint32_t error_capacity)
{
    struct rk_encoder *enc = opaque;
    (void)error;
    (void)error_capacity;
    if (enc == NULL) {
        return -1;
    }
    pthread_mutex_lock(&enc->mutex);
    enc->force_keyframe = true;
#ifdef ONEKVM_HAVE_ROCKIT
    if (enc->venc_started) {
        RK_MPI_VENC_RequestIDR(enc->venc_chn, RK_TRUE);
    }
#endif
#ifndef ONEKVM_HAVE_ROCKIT
    if (enc->vcodec_fd >= 0) {
        (void)kmpp_ioctl(enc->vcodec_fd, KMPP_VCODEC_CHAN_CONTROL, KMPP_ENC_SET_IDR_FRAME, NULL,
                         0);
    }
#endif
    pthread_mutex_unlock(&enc->mutex);
    return 0;
}

uint32_t rk_encoder_codec(void *opaque)
{
    struct rk_encoder *enc = opaque;
    return enc == NULL ? ONEKVM_VIDEO_CODEC_H264 : enc->config.codec;
}

void rk_encoder_destroy(void *opaque)
{
    struct rk_encoder *enc = opaque;
    if (enc == NULL) {
        return;
    }
    pthread_mutex_lock(&enc->mutex);
    stop_bound_locked(enc);
#ifdef ONEKVM_HAVE_ROCKIT
    if (enc->sys_inited) {
        RK_MPI_SYS_Exit();
        enc->sys_inited = false;
    }
#endif
    au_owned_release_locked(enc, &enc->borrowed, &enc->borrowed_slab);
    enc->borrowed_size = 0;
    queue_clear_locked(enc);
    {
        unsigned si;

        for (si = 0; si < RK_AU_SLABS; si++) {
            free(enc->au_slab[si]);
            enc->au_slab[si] = NULL;
            enc->au_slab_busy[si] = 0;
        }
    }
    free(enc->stream_buf);
    enc->stream_buf = NULL;
    enc->stream_cap = 0;
    pthread_mutex_unlock(&enc->mutex);
    pthread_mutex_lock(&g_resource_lock);
    if (g_active_h26x > 0) {
        g_active_h26x--;
    }
    pthread_mutex_unlock(&g_resource_lock);
    pthread_cond_destroy(&enc->ready);
    pthread_mutex_destroy(&enc->mutex);
#ifndef ONEKVM_HAVE_ROCKIT
    close(enc->capture_wake);
    close(enc->drain_wake);
#endif
    free(enc);
}

void rk_encoder_release_packet(void *opaque)
{
    struct rk_encoder *enc = opaque;
    if (enc == NULL) {
        return;
    }
    pthread_mutex_lock(&enc->mutex);
    au_owned_release_locked(enc, &enc->borrowed, &enc->borrowed_slab);
    enc->borrowed_size = 0;
    pthread_mutex_unlock(&enc->mutex);
}

int32_t rk_encoder_bind_source(void *opaque, void *source, char *error, uint32_t error_capacity)
{
    struct rk_encoder *enc = opaque;

    if (enc == NULL || source == NULL) {
        onekvm_set_error(error, error_capacity, "invalid bind");
        return -1;
    }
    pthread_mutex_lock(&enc->mutex);
    /* A capture thread that returned on its own leaves nothing to read:
     * restart it instead of reporting the dead binding as bound. */
    if (enc->capture_started && enc->bound_source == source &&
        !atomic_load(&enc->capture_exited)) {
        pthread_mutex_unlock(&enc->mutex);
        return 0;
    }
    if (enc->capture_started) {
        stop_bound_locked(enc);
    }
    if (!rk_source_capture_claim(source, enc)) {
        pthread_mutex_unlock(&enc->mutex);
        onekvm_set_error(error, error_capacity, "source capture is already owned");
        return ONEKVM_VIDEO_RESOURCE_BUSY;
    }
    enc->bound_source = source;
    enc->stop = false;
    atomic_store(&enc->capture_exited, false);
#ifndef ONEKVM_HAVE_ROCKIT
    enc->scale_disabled = false;
#endif
    if (pthread_create(&enc->capture_thread, NULL, capture_drain, enc) != 0) {
        rk_source_capture_release(source, enc);
        enc->bound_source = NULL;
        pthread_mutex_unlock(&enc->mutex);
        onekvm_set_error(error, error_capacity, "start capture thread: %s", strerror(errno));
        return -1;
    }
    enc->capture_started = true;
    pthread_mutex_unlock(&enc->mutex);
    return 0;
}

int32_t rk_encoder_read_packet(void *opaque, struct onekvm_video_packet_v1 *packet, char *error,
                               uint32_t error_capacity)
{
    struct rk_encoder *enc = opaque;
    struct timespec deadline;
    struct rk_au *slot;

    if (enc == NULL || packet == NULL || packet->struct_size < sizeof(*packet)) {
        onekvm_set_error(error, error_capacity, "invalid packet read");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)RK_PACKET_WAIT_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&enc->mutex);
    enc->last_read_ns = monotonic_ns();
    while (enc->q_count == 0 && !enc->stop) {
        if (pthread_cond_timedwait(&enc->ready, &enc->mutex, &deadline) == ETIMEDOUT) {
            packet->data = NULL;
            packet->data_size = 0;
            packet->codec = enc->config.codec;
            packet->key_frame = 0;
            packet->pts_ns = 0;
            pthread_mutex_unlock(&enc->mutex);
            return 0;
        }
    }
    if (enc->q_count == 0) {
        packet->data = NULL;
        packet->data_size = 0;
        packet->codec = enc->config.codec;
        pthread_mutex_unlock(&enc->mutex);
        return 0;
    }
    slot = &enc->queue[enc->q_head];
    au_owned_release_locked(enc, &enc->borrowed, &enc->borrowed_slab);
    enc->borrowed = slot->data;
    enc->borrowed_size = slot->size;
    enc->borrowed_slab = slot->slab;
    packet->data = enc->borrowed;
    packet->data_size = slot->size;
    packet->codec = enc->config.codec;
    packet->key_frame = slot->key_frame;
    packet->pts_ns = slot->pts_ns;
#ifndef ONEKVM_HAVE_ROCKIT
    if (slot->key_frame) {
        atomic_store(&enc->color_info, RK_BIT_DEPTH | slot->color_range << 8);
    }
#endif
    slot->data = NULL;
    slot->size = 0;
    slot->slab = -1;
    enc->q_head = (enc->q_head + 1) % RK_PACKET_QUEUE;
    enc->q_count--;
    pthread_mutex_unlock(&enc->mutex);
    (void)error;
    (void)error_capacity;
    return 0;
}

int32_t rk_encoder_unbind_source(void *opaque, char *error, uint32_t error_capacity)
{
    struct rk_encoder *enc = opaque;
    (void)error;
    (void)error_capacity;
    if (enc == NULL) {
        return 0;
    }
    pthread_mutex_lock(&enc->mutex);
    stop_bound_locked(enc);
    pthread_mutex_unlock(&enc->mutex);
    return 0;
}

int32_t rk_encoder_latency(void *opaque, struct onekvm_video_latency_v1 *latency)
{
    struct rk_encoder *enc = opaque;

    if (enc == NULL) {
        return -1;
    }
    return onekvm_fill_latency(latency, onekvm_load_stage_ns(&enc->last_capture_ns),
                               onekvm_load_stage_ns(&enc->last_encode_ns));
}

int32_t rk_encoder_color_info(void *opaque, struct onekvm_video_color_info_v1 *info)
{
    struct rk_encoder *enc = opaque;
    uint32_t cached;

    if (enc == NULL || info == NULL || info->struct_size < sizeof(*info)) {
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    cached = atomic_load(&enc->color_info);
    info->bit_depth = cached & 0xffu;
    info->color_range = cached >> 8;
    return ONEKVM_VIDEO_RESOURCE_OK;
}

int32_t rk_encoder_resources(struct onekvm_video_encoder_resources_v1 *resources, char *error,
                            uint32_t error_capacity)
{
    (void)error;
    (void)error_capacity;
    if (resources == NULL || resources->struct_size < sizeof(*resources)) {
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    pthread_mutex_lock(&g_resource_lock);
    resources->policy_h26x_capacity = 1;
    resources->policy_jpeg_capacity = 0;
    resources->active_h26x = g_active_h26x;
    resources->active_jpeg = 0;
    resources->hardware_channel_capacity = 1;
    resources->input_mode_mask = ONEKVM_VIDEO_ENCODER_INPUT_MASK_BOUND;
    resources->purpose_mask = ONEKVM_VIDEO_ENCODER_PURPOSE_MASK_REALTIME;
    resources->flags = 0;
    pthread_mutex_unlock(&g_resource_lock);
    return ONEKVM_VIDEO_RESOURCE_OK;
}
