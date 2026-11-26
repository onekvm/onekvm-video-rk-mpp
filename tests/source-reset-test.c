#include <assert.h>
#include "../src/v4l2_mplane.c"

int main(void)
{
    struct rk_source source = {0};
    struct onekvm_video_source_config_v1 config = {.struct_size = sizeof(config)};
    char error[256];
    int width;
    int height;

    assert(onekvm_map_resolution(6400480, &width, &height));
    assert(width == 640 && height == 480);
    assert(!onekvm_map_resolution(6410480, &width, &height));
    assert(!onekvm_map_resolution(-1, &width, &height));
    assert(onekvm_scale_supported(1920, 1080, 640, 480));
    assert(!onekvm_scale_supported(640, 480, 1280, 720));
    assert(!onekvm_scale_supported(1920, 1080, 160, 120));
    pthread_mutex_init(&source.mutex, NULL);
    source.video_fd = -1;
    source.subdev_fd = -1;
    source.width = 1920;
    source.height = 1080;
    source.signal = true;
    source.streaming = true;
    source.want_width = 1280;
    source.want_height = 720;
    config.resolution = 6410480;
    assert(rk_source_reset(&source, &config, error, sizeof(error)) == ONEKVM_VIDEO_RESOURCE_UNSUPPORTED);
    assert(source.streaming && source.want_width == 1280 && source.width == 1920);
    assert(strstr(error, "unsupported output size") != NULL);
    config.resolution = 19201200;
    assert(rk_source_reset(&source, &config, error, sizeof(error)) != 0);
    assert(source.streaming && source.want_width == 1280);
    assert(rk_source_reset(&source, NULL, error, sizeof(error)) != 0);
    assert(rk_source_output_size(&source, 1920, 1080, &width, &height) == 1);
    assert(width == 1280 && height == 720);
    source.streaming = false;
    config.resolution = 0;
    assert(rk_source_reset(&source, &config, error, sizeof(error)) == 0);
    assert(rk_source_output_size(&source, 1920, 1080, &width, &height) == 0);
    {
        /* Status and per-packet queries reuse fresh timings without I2C. */
        struct onekvm_video_format_v1 format = {.struct_size = sizeof(format)};

        source.width = 1280;
        source.height = 720;
        source.fps = 60;
        source.signal = true;
        publish_timings_locked(&source);
        source.width = 1920;
        assert(rk_source_input_format(&source, &format) == 0);
        assert(format.width == 1280 && format.height == 720 && format.fps == 60);
        assert(format.flags == 0 && rk_source_signal_present(&source) == 1);
        /* Stale: query again (no subdev here, so no signal). */
        atomic_store(&source.timings_ns, monotonic_ns() - RK_TIMINGS_CACHE_NS);
        assert(rk_source_input_format(&source, &format) == 0);
        assert(format.flags == ONEKVM_VIDEO_FORMAT_OUT_OF_RANGE);
        assert(rk_source_signal_present(&source) == 0);
    }
    pthread_mutex_destroy(&source.mutex);
    return 0;
}
