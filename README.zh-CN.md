# onekvm-video-rk-mpp

[English](README.md) | 简体中文

OneKVM 的 Rockchip MPP 原生视频后端（PicoKVM / RV1106，JetKVM 可共用）。
Core 通过 `onekvm_video_backend_query` 加载
`/usr/lib/onekvm/video-backends/rk-mpp.so`。采集是 TC358743 或 RK628F → CIF 的 V4L2
MPLANE。硬件编码是 Rockchip MPI VENC（`librockit`），需
`-DONEKVM_HAVE_ROCKIT=ON`。

本仓库不含 HID / USB gadget。descriptor 的 `id` 是机型（`picokvm`），
`driver` 才是栈（`rk-mpp`）。

通路说明见 [docs/video-pipeline.md](docs/video-pipeline.md)。

## 许可证

MIT。vendor `librockit` 仍是专有库，不从本树打包。
