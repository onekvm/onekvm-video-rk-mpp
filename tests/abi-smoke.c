/* SPDX-License-Identifier: MIT */

#include <onekvm/video_backend_v1.h>

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void fail(const char *operation, const char *detail)
{
    fprintf(stderr, "%s: %s\n", operation, detail && detail[0] ? detail : "failed");
}

int main(int argc, char **argv)
{
    void *library;
    onekvm_video_backend_query_fn query;
    const void *raw_api = NULL;
    const struct onekvm_video_backend_v1 *api;
    char error[256];
    struct onekvm_video_encoder_config_v1 encoder_config;
    struct onekvm_video_source_config_v1 source_config;
    void *encoder = NULL;
    void *source = NULL;

    union {
        void *symbol;
        onekvm_video_backend_query_fn fn;
    } loaded;

    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/rk-mpp.so\n", argv[0]);
        return 1;
    }
    library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fail("dlopen", dlerror());
        return 1;
    }
    loaded.symbol = dlsym(library, ONEKVM_VIDEO_BACKEND_QUERY_SYMBOL);
    query = loaded.fn;
    if (query == NULL) {
        fail("dlsym", dlerror());
        return 1;
    }
    if (query(99u, &raw_api) == 0 || raw_api != NULL) {
        fail("query", "ABI 99 must be rejected");
        return 1;
    }
    if (query(ONEKVM_VIDEO_BACKEND_ABI_V1, &raw_api) != 0 || raw_api == NULL) {
        fail("query", "ABI v1 unavailable");
        return 1;
    }
    api = raw_api;
    if (api->driver_id == NULL || strcmp(api->driver_id, "rk-mpp") != 0) {
        fail("driver_id", api->driver_id ? api->driver_id : "null");
        return 1;
    }
    if (!(api->features & ONEKVM_VIDEO_FEATURE_BOUND_ENCODER) ||
        api->encoder_bind_source == NULL || api->encoder_read_packet == NULL ||
        api->encoder_unbind_source == NULL) {
        fail("features", "bound encoder ABI missing");
        return 1;
    }
    if (!(api->features & ONEKVM_VIDEO_FEATURE_LATENCY) || api->source_latency == NULL ||
        api->encoder_latency == NULL) {
        fail("features", "latency ABI missing");
        return 1;
    }
    if (!(api->features & ONEKVM_VIDEO_FEATURE_ENCODER_COLOR_INFO) ||
        api->encoder_color_info == NULL) {
        fail("features", "encoder color info missing");
        return 1;
    }
    memset(&encoder_config, 0, sizeof(encoder_config));
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.codec = ONEKVM_VIDEO_CODEC_H264;
    encoder_config.quality_factor = 0.8;
    encoder_config.fps = 60;
    if (api->encoder_create(&encoder_config, &encoder, error, sizeof(error)) != 0 ||
        encoder == NULL) {
        fail("encoder_create", error);
        return 1;
    }
    memset(&source_config, 0, sizeof(source_config));
    source_config.struct_size = sizeof(source_config);
    source_config.device = "/dev/null";
    if (api->source_create(&source_config, &source, error, sizeof(error)) != 0 || source == NULL) {
        fail("source_create", error);
        api->encoder_destroy(encoder);
        return 1;
    }
    {
        struct onekvm_video_latency_v1 latency;

        memset(&latency, 0, sizeof(latency));
        latency.struct_size = sizeof(latency);
        if (api->source_latency(source, &latency) != 0 || latency.capture_ns != 0 ||
            latency.encode_ns != 0) {
            fail("source_latency", "expected zero samples before capture");
            api->source_destroy(source);
            api->encoder_destroy(encoder);
            return 1;
        }
        memset(&latency, 0, sizeof(latency));
        latency.struct_size = sizeof(latency);
        if (api->encoder_latency(encoder, &latency) != 0 || latency.capture_ns != 0 ||
            latency.encode_ns != 0) {
            fail("encoder_latency", "expected zero samples before encode");
            api->source_destroy(source);
            api->encoder_destroy(encoder);
            return 1;
        }
    }
    {
        struct onekvm_video_color_info_v1 color;

        memset(&color, 0, sizeof(color));
        color.struct_size = sizeof(color);
        if (api->encoder_color_info(encoder, &color) != 0 || color.bit_depth != 0 ||
            color.color_range != ONEKVM_VIDEO_COLOR_RANGE_UNKNOWN) {
            fail("encoder_color_info", "expected unknown before the first keyframe");
            api->source_destroy(source);
            api->encoder_destroy(encoder);
            return 1;
        }
    }
    api->source_destroy(source);
    api->encoder_destroy(encoder);
    dlclose(library);
    return 0;
}
