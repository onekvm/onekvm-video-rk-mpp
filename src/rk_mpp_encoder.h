#ifndef ONEKVM_VIDEO_RK_MPP_ENCODER_H
#define ONEKVM_VIDEO_RK_MPP_ENCODER_H

#include <onekvm/video_backend_v1.h>

int32_t rk_encoder_create(const struct onekvm_video_encoder_config_v1 *config, void **encoder,
                          char *error, uint32_t error_capacity);
int32_t rk_encoder_reset(void *encoder, const struct onekvm_video_encoder_config_v1 *config,
                         char *error, uint32_t error_capacity);
int32_t rk_encoder_encode(void *encoder, const struct onekvm_video_frame_v1 *frame,
                          struct onekvm_video_packet_v1 *packet, char *error,
                          uint32_t error_capacity);
int32_t rk_encoder_set_quality(void *encoder, double quality_factor, char *error,
                              uint32_t error_capacity);
int32_t rk_encoder_request_keyframe(void *encoder, char *error, uint32_t error_capacity);
uint32_t rk_encoder_codec(void *encoder);
void rk_encoder_destroy(void *encoder);
void rk_encoder_release_packet(void *encoder);
int32_t rk_encoder_bind_source(void *encoder, void *source, char *error, uint32_t error_capacity);
int32_t rk_encoder_read_packet(void *encoder, struct onekvm_video_packet_v1 *packet, char *error,
                               uint32_t error_capacity);
int32_t rk_encoder_unbind_source(void *encoder, char *error, uint32_t error_capacity);
int32_t rk_encoder_resources(struct onekvm_video_encoder_resources_v1 *resources, char *error,
                            uint32_t error_capacity);
int32_t rk_encoder_latency(void *encoder, struct onekvm_video_latency_v1 *latency);
int32_t rk_encoder_color_info(void *encoder, struct onekvm_video_color_info_v1 *info);

#endif
