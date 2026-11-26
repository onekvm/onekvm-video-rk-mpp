/* SPDX-License-Identifier: MIT */
#ifndef ONEKVM_VIDEO_RK_MPP_KMPP_VCODEC_H
#define ONEKVM_VIDEO_RK_MPP_KMPP_VCODEC_H

#include <linux/ioctl.h>
#include <stddef.h>
#include <stdint.h>

#define KMPP_VOCDEC_IOC_MAGIC 'V'
#define KMPP_VOCDEC_IOC_CFG _IOW(KMPP_VOCDEC_IOC_MAGIC, 1, unsigned int)

#define KMPP_VCODEC_ID_BASE_COMMON 0x00000000u
#define KMPP_VCODEC_ID_BASE_STATE 0x00000100u
#define KMPP_VCODEC_ID_BASE_INPUT 0x00000400u
#define KMPP_VCODEC_ID_BASE_OUTPUT 0x00000600u
#define KMPP_VCODEC_ID_BASE_OUTPUT_ACK 0x00000700u

#define KMPP_VCODEC_CHAN_CREATE (KMPP_VCODEC_ID_BASE_COMMON + 0u)
#define KMPP_VCODEC_CHAN_DESTROY (KMPP_VCODEC_ID_BASE_COMMON + 1u)
#define KMPP_VCODEC_CHAN_CONTROL (KMPP_VCODEC_ID_BASE_COMMON + 3u)
#define KMPP_VCODEC_CHAN_START (KMPP_VCODEC_ID_BASE_STATE + 0u)
#define KMPP_VCODEC_CHAN_STOP (KMPP_VCODEC_ID_BASE_STATE + 1u)
#define KMPP_VCODEC_CHAN_IN_FRM_RDY (KMPP_VCODEC_ID_BASE_INPUT + 0u)
#define KMPP_VCODEC_CHAN_OUT_STRM_BUF_RDY (KMPP_VCODEC_ID_BASE_OUTPUT + 1u)
#define KMPP_VCODEC_CHAN_OUT_STRM_END (KMPP_VCODEC_ID_BASE_OUTPUT + 2u)
#define KMPP_VCODEC_CHAN_ONEKVM_SETUP (KMPP_VCODEC_ID_BASE_OUTPUT_ACK + 16u)
#define KMPP_VCODEC_CHAN_ONEKVM_ALLOC_DMA (KMPP_VCODEC_ID_BASE_OUTPUT_ACK + 17u)
#define KMPP_VCODEC_CHAN_ONEKVM_RATE (KMPP_VCODEC_ID_BASE_OUTPUT_ACK + 18u)

#define KMPP_CTX_ENC 1
#define KMPP_VIDEO_CODING_AVC 7
#define KMPP_VIDEO_CODING_MJPEG 8
#define KMPP_VIDEO_CODING_HEVC 0x01000004u
#define KMPP_FMT_YUV422SP 2u
#define KMPP_FMT_YUV420SP 0u
#define KMPP_FMT_YUV422_YUYV 8u
#define KMPP_FMT_YUV422_UYVY 10u
#define KMPP_PACKET_FLAG_INTRA 0x00000010u
#define KMPP_ENC_SET_IDR_FRAME 0x00320009u
#define KMPP_ENC_SET_CODEC_CFG 0x00320007u
/* MppFrameColorRange and MppFrameColorSpace. */
#define KMPP_RANGE_MPEG 1u
#define KMPP_RANGE_JPEG 2u
#define KMPP_SPC_BT709 1u
#define KMPP_SPC_SMPTE170M 6u

struct kmpp_jpeg_quality {
    uint32_t coding;
    uint32_t change;
    int32_t quant;
    int32_t q_factor;
    int32_t qf_max;
    int32_t qf_min;
    uint32_t qtable_y;
    uint32_t qtable_u;
    uint32_t qtable_v;
    uint8_t reserved[2832u - 36u];
};

_Static_assert(sizeof(struct kmpp_jpeg_quality) == 2832u, "RV1106 codec config ABI");

struct kmpp_vcodec_msg {
    uint32_t cmd;
    uint32_t ctrl_cmd;
    uint32_t size;
    uint64_t data_ptr;
};

struct kmpp_vcodec_attr {
    int32_t type;
    int32_t coding;
    int32_t chan_id;
    int32_t online;
    uint32_t buf_size;
    uint32_t max_strm_cnt;
    uint32_t shared_buf_en;
    uint32_t smart_en;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_lt_cnt;
    uint32_t qpmap_en;
    uint32_t chan_dup;
    uint32_t res;
    uint32_t only_smartp;
};

struct kmpp_onekvm_setup {
    uint32_t width;
    uint32_t height;
    uint32_t hor_stride;
    uint32_t ver_stride;
    uint32_t fmt;
    uint32_t fps;
    uint32_t bps;
    uint32_t gop;
    uint32_t coding;
    /* VUI only; 0 leaves each unspecified. Older kernels take V1_SIZE. */
    uint32_t range;
    uint32_t matrix;
    uint32_t primaries;
    uint32_t transfer;
};

#define KMPP_ONEKVM_SETUP_V1_SIZE offsetof(struct kmpp_onekvm_setup, range)

struct kmpp_dma_alloc {
    uint32_t size;
    int32_t fd;
};

struct kmpp_onekvm_rate {
    uint32_t bps;
    int32_t initial_qp;
    int32_t min_qp;
    int32_t max_qp;
};

struct kmpp_frame_infos {
    uint32_t width;
    uint32_t height;
    uint32_t hor_stride;
    uint32_t ver_stride;
    uint32_t hor_stride_pixel;
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t fmt;
    uint32_t fd;
    uint64_t pts;
    int32_t jpeg_chan_id;
    void *osd_buf;
    int32_t mpi_buf_id;
    void *jpg_combo_osd_buf;
    uint32_t is_gray;
    uint32_t is_full;
    uint32_t phy_addr;
    uint64_t dts;
    void *pp_info;
    uint32_t res[2];
};

struct kmpp_venc_pack_info {
    uint32_t flag;
    uint32_t temporal_id;
    uint32_t packet_offset;
    uint32_t packet_len;
};

struct kmpp_venc_packet {
    int64_t u64priv_data;
    uint64_t u64packet_addr;
    uint32_t len;
    uint32_t buf_size;
    uint64_t u64pts;
    uint64_t u64dts;
    uint32_t flag;
    uint32_t temporal_id;
    uint32_t offset;
    uint32_t data_num;
    struct kmpp_venc_pack_info packet[8];
};

#endif
