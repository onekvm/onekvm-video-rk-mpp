# onekvm-video-rk-mpp

English | [简体中文](README.zh-CN.md)

Rockchip MPP native video backend for OneKVM (PicoKVM / RV1106, also usable
on JetKVM). Core loads `/usr/lib/onekvm/video-backends/rk-mpp.so` through
`onekvm_video_backend_query`. Capture is V4L2 MPLANE from TC358743 or RK628F → CIF.
Hardware encode is Rockchip MPI VENC (`librockit`) when built with
`ONEKVM_HAVE_ROCKIT`.

This repository does not contain HID or USB gadget code. Descriptor `id` is
the machine (`picokvm`); `driver` is the stack (`rk-mpp`).

See [docs/video-pipeline.md](docs/video-pipeline.md).

## License

MIT. Vendor `librockit` remains proprietary and is not shipped from this tree.
