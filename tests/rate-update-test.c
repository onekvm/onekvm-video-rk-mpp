#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include "kmpp_vcodec.h"

static unsigned updates;
static int fail_update;
static struct kmpp_onekvm_rate last_rate;

static int test_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    struct kmpp_vcodec_msg *message;

    assert(fd == 42);
    assert(request == KMPP_VOCDEC_IOC_CFG);
    va_start(arguments, request);
    message = va_arg(arguments, struct kmpp_vcodec_msg *);
    va_end(arguments);
    assert(message->cmd == KMPP_VCODEC_CHAN_ONEKVM_RATE);
    assert(message->size == sizeof(last_rate));
    last_rate = *(struct kmpp_onekvm_rate *)(uintptr_t)message->data_ptr;
    updates++;
    return fail_update;
}

#define ioctl test_ioctl
#include "../src/rk_mpp_encoder.c"
#undef ioctl

int main(void)
{
    /* Reader activity: a read newer than the captured frame is active. */
    assert(!reader_active(0, 1000));
    assert(reader_active(1000, 1000));
    assert(reader_active(2000, 1000));
    assert(reader_active(1000, 1000 + RK_VEPU_IDLE_NS - 1));
    assert(!reader_active(1000, 1000 + RK_VEPU_IDLE_NS));

    struct rk_encoder encoder = {0};
    struct onekvm_video_encoder_config_v1 config = {
        .struct_size = sizeof(config), .codec = ONEKVM_VIDEO_CODEC_H264,
        .fps = 60, .gop = 60, .quality_factor = 0.5,
    };
    char error[128];

    pthread_mutex_init(&encoder.mutex, NULL);
    encoder.config = config;
    encoder.vcodec_fd = 42;
    encoder.active_width = 1920;
    encoder.active_height = 1080;
    encoder.capture_started = true;
    encoder.bound_source = &encoder;
    encoder.q_count = 2;
    config.bitrate_kbps = 1500;
    config.min_qp = 14;
    config.max_qp = 45;
    assert(rk_encoder_reset(&encoder, &config, error, sizeof(error)) == 0);
    assert(updates == 1 && last_rate.bps == 1500000);
    assert(last_rate.min_qp == 14 && last_rate.max_qp == 45);
    assert(encoder.capture_started && encoder.bound_source == &encoder);
    assert(encoder.q_count == 2 && encoder.vcodec_fd == 42);
    assert(rk_encoder_set_quality(&encoder, 0.7, error, sizeof(error)) == 0);
    assert(updates == 2 && encoder.config.quality_factor == 0.7);
    fail_update = -1;
    config.bitrate_kbps = 2200;
    assert(rk_encoder_reset(&encoder, &config, error, sizeof(error)) == -1);
    assert(encoder.config.bitrate_kbps == 1500 && encoder.capture_started);
    encoder.capture_started = false;
    encoder.vcodec_fd = -1;
    fail_update = 0;
    config.codec = ONEKVM_VIDEO_CODEC_H265;
    assert(rk_encoder_reset(&encoder, &config, error, sizeof(error)) == 0);
    assert(encoder.config.codec == ONEKVM_VIDEO_CODEC_H265 && updates == 3);
    pthread_mutex_destroy(&encoder.mutex);
    {
        void *opaque;
        struct rk_encoder *worker;
        int descriptors[2];
        uint64_t started;

        assert(rk_encoder_create(&config, &opaque, error, sizeof(error)) == 0);
        worker = opaque;
        assert(pipe(descriptors) == 0);
        worker->vcodec_fd = descriptors[0];
        worker->stream_cap = 1;
        assert(pthread_create(&worker->drain_thread, NULL, kmpp_drain_thread, worker) == 0);
        usleep(10000);
        started = monotonic_ns();
        atomic_store(&worker->drain_stop, true);
        wake_fd(worker->drain_wake);
        pthread_join(worker->drain_thread, NULL);
        assert(monotonic_ns() - started < 100000000ull);
        close(descriptors[0]);
        close(descriptors[1]);
        worker->vcodec_fd = -1;
        rk_encoder_destroy(worker);
    }
    return 0;
}
