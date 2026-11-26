/* SPDX-License-Identifier: MIT */

#include "internal.h"
#include "rk_mpp_encoder.h"
#include "v4l2_mplane.h"
#include "snapshot.h"

static const struct onekvm_video_format_v1 kFormats[] = {
    {sizeof(struct onekvm_video_format_v1), 1920, 1080, 60, ONEKVM_VIDEO_PIXEL_UNKNOWN, 0},
    {sizeof(struct onekvm_video_format_v1), 1280, 720, 60, ONEKVM_VIDEO_PIXEL_UNKNOWN, 0},
    {sizeof(struct onekvm_video_format_v1), 640, 480, 30, ONEKVM_VIDEO_PIXEL_UNKNOWN, 0},
};

/* The ROCKIT VENC's stream signalling is not checked; only kmpp reports color. */
#ifdef ONEKVM_HAVE_ROCKIT
#define RK_FEATURE_COLOR_INFO 0
#else
#define RK_FEATURE_COLOR_INFO ONEKVM_VIDEO_FEATURE_ENCODER_COLOR_INFO
#endif

static const struct onekvm_video_backend_v1 kBackend = {
    .struct_size = sizeof(struct onekvm_video_backend_v1),
    .abi_version = ONEKVM_VIDEO_BACKEND_ABI_V1,
    .driver_id = ONEKVM_RK_MPP_DRIVER_ID,
    .codec_mask = ONEKVM_VIDEO_CODEC_MASK_AUTO | ONEKVM_VIDEO_CODEC_MASK_H264 |
                  ONEKVM_VIDEO_CODEC_MASK_H265,
    .features = ONEKVM_VIDEO_FEATURE_SIGNAL_PRESENT | ONEKVM_VIDEO_FEATURE_KEYFRAME |
                ONEKVM_VIDEO_FEATURE_INTERRUPT_READ | ONEKVM_VIDEO_FEATURE_BOUND_ENCODER |
                ONEKVM_VIDEO_FEATURE_BORROWED_PACKET | ONEKVM_VIDEO_FEATURE_EDID |
                ONEKVM_VIDEO_FEATURE_LATENCY | ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY |
                ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY_RGA |
                ONEKVM_VIDEO_FEATURE_BOUND_ENCODED_TIME | ONEKVM_VIDEO_FEATURE_SOURCE_SNAPSHOT |
                ONEKVM_VIDEO_FEATURE_SOURCE_RESET_WHILE_BOUND | RK_FEATURE_COLOR_INFO,
    .formats = kFormats,
    .format_count = (uint32_t)(sizeof(kFormats) / sizeof(kFormats[0])),
    .source_create = rk_source_create,
    .source_reset = rk_source_reset,
    .source_read = rk_source_read,
    .source_release = rk_source_release,
    .source_signal_present = rk_source_signal_present,
    .source_interrupt = rk_source_interrupt,
    .source_destroy = rk_source_destroy,
    .encoder_create = rk_encoder_create,
    .encoder_reset = rk_encoder_reset,
    .encoder_encode = rk_encoder_encode,
    .encoder_set_quality = rk_encoder_set_quality,
    .encoder_request_keyframe = rk_encoder_request_keyframe,
    .encoder_codec = rk_encoder_codec,
    .encoder_destroy = rk_encoder_destroy,
    .encoder_release_packet = rk_encoder_release_packet,
    .encoder_prepare = NULL,
    .encoder_bind_source = rk_encoder_bind_source,
    .encoder_read_packet = rk_encoder_read_packet,
    .encoder_unbind_source = rk_encoder_unbind_source,
    .source_input_format = rk_source_input_format,
    .source_latency = rk_source_latency,
    .encoder_latency = rk_encoder_latency,
    .edid_capabilities = rk_edid_capabilities,
    .edid_get = rk_edid_get,
    .edid_set = rk_edid_set,
    .encoder_resources = rk_encoder_resources,
    .encoder_allocate = NULL,
    .encoder_allocation = NULL,
    .source_snapshot = rk_source_snapshot,
    .source_display = rk_source_display,
    .encoder_color_info = rk_encoder_color_info,
    .edid_set_mode = rk_edid_set_mode,
};

ONEKVM_VIDEO_EXPORT int32_t onekvm_video_backend_query(uint32_t requested_abi,
                                                       const void **backend_api)
{
    if (backend_api == NULL) {
        return -1;
    }
    *backend_api = NULL;
    if (requested_abi != ONEKVM_VIDEO_BACKEND_ABI_V1) {
        return -1;
    }
    *backend_api = &kBackend;
    return 0;
}
