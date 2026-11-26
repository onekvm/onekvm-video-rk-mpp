#ifndef ONEKVM_VIDEO_RK_MPP_RGA_SCALE_H
#define ONEKVM_VIDEO_RK_MPP_RGA_SCALE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct rk_rga_dma;
struct rk_rga_dma *rk_rga_dma_open(uint32_t fourcc, int width, int height,
                                   uint32_t stride, int dw, int dh, size_t *bytes);
int rk_rga_dma_run(struct rk_rga_dma *rga, int src_fd, size_t src_bytes,
                    int dst_fd, int timeout_ms);
void rk_rga_dma_close(struct rk_rga_dma *rga);

/* Mainline rockchip-rga V4L2 m2m: NV16/NV12 → RGB565. Packed YUYV is not
 * in that driver's 4-bit RGA1 format field, so CIF should prefer NV16
 * when this device is present. encoding/quantization are the source's
 * V4L2 YCbCr encoding and range (DEFAULT when unknown). */
bool rk_rga_present(void);
int rk_rga_scale_rgb565(int src_fd, uint32_t fourcc, int32_t width, int32_t height,
                        uint32_t stride, uint32_t encoding, uint32_t quantization,
                        uint16_t *dst, int32_t dw, int32_t dh);
/* Release the source buffer the display session still references. Call
 * before freeing capture buffers. */
void rk_rga_scale_release(void);

#endif
