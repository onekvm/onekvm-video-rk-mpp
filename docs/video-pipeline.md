# PicoKVM HDMI 采集与 rk-mpp

PicoKVM（RV1106）的 Core 加载
`/usr/lib/onekvm/video-backends/rk-mpp.so`。

```text
HDMI → TC358743 / RK628F → CSI2 DPHY → RKCIF /dev/video0 (MPLANE NV16 when RGA is up, else YUYV)
                 → rk-mpp.so  (encoder_bind_source + encoder_read_packet)
                   输出尺寸与输入不同时先经 RGA m2m NV16→NV12 再送 VEPU
                 → onekvm-core → WebRTC
                 display: CIF DMABUF → rockchip-rga m2m NV16→RGB565 640×360
                 （主线 RGA 没有 packed YUYV；RGA 不在就回退 A7 最近邻）
                 rk-mpp.so 也导出 crypto ABI v1（`srtp-tx-offload`），
                 走 `/dev/onekvm-crypto-offload` → CE v3 AES-GCM。
```

不要用 `onekvm-generic-v4l2`：那是单平面 NV12 + x264，CIF 是 MPLANE，A7 也扛不住 1080p60 软编。

CIF 使用 NV16 接收桥片的 YUV422 数据。启用 NV12 之前，需确认 CIF 支持
4:2:0 色度抽样，并按 `QUERYBUF` 返回的长度核对写入范围。

## Bound 路径

PicoKVM KMPP 的纯码率／quality／QP 更新通过 `ONEKVM_RATE` 原地修改运行通道。
更新失败返回错误，保留旧配置，不通过重建掩盖不匹配的内核 ABI；需配套内核。
codec、fps、GOP、输出尺寸改动仍走重启；源时序变化也重启。ROCKIT 保留原重启路径。
默认 QP 为 initial=22、min=12、max=48（I 帧 max=46）；显式 QP 同时用于启动与运行更新。
设备验收须在同一播放器制造反复码率上限变化，核对 `rate update` 连续出现而
`kmpp open` 不增加；比较实际码率、解码 FPS、冻结及源→浏览器 P50/P95。
主机 ioctl mock 仅验证不重启、不清队列及失败时保留配置，不证明硬件收益。

Core 的 native encoder 一律走 `as_bound()`。本后端必须实现：

- `encoder_bind_source`：等 HDMI `QUERY_DV_TIMINGS`，打开 `/dev/video0`，
  `VIDIOC_STREAMON`。真正采集在 `capture_drain`（不是 `source_read`）。
- `encoder_read_packet`：从 `/dev/vcodec` AU 队列取包。采集停止期间（启动时无
  HDMI、失锁、重启采集失败）由 VEPU 编码无信号画面（见下节）；采集恢复后按采集
  尺寸重开 VEPU，并清掉断流前的旧 AU。丢掉启动前两帧和 bytesused 过小的残帧，再
  `IN_FRM_RDY` 给 VEPU。
- CIF 缓冲对齐 JetKVM：`VIDIOC_REQBUFS` **DMABUF**，先 `QUERYBUF` 再按
  `planes[0].length` `ALLOC_DMA`（CIF `queue_setup` 高度 16 对齐，1080p 是
  `3840×1088` 不是 S_FMT `sizeimage`）。拿不到再回退 CIF MMAP+`EXPBUF`。
  NV16/NV12 的 VEPU `ver_stride` 用真实高度（1080），不要 16 对齐：CIF UV
  在 `bytesperline×height`，对齐成 1088 会把色度错 8 行并在底部留下绿条。
  Packed YUYV 没有独立 UV，仍用 16 对齐高度。
-   独立 drain 线程等待 `/dev/vcodec` 就绪后 `OUT_STRM_BUF_RDY`，200 ms
  watchdog；eventfd 打断停止／重启。CIF 等待图像、HDMI 事件及 AU 完成通知，
  100 ms watchdog 保留时序／无信号检测；条件变量统一用单调时钟，完成用 broadcast。
  需配套使用原子 stream_count 判断就绪的内核；队列深度不变。编码帧率跟 HDMI
  refresh（1080p60）。CIF **4** 块 VEPU CMA DMABUF，驱动里保持 **2** 块
  （1 块在途编码、1 块 spare）。只要 1 块会错过 SOF、掉到 30 fps。DQ 后
  立刻补 QBUF。采集延迟用 CIF FS 时间戳→DQ（一帧填充），不是 QBUF 驻留。
  `IN_FRM_RDY` 后等 AU 再还槽，在途最多 **1** 块。8 块全入队时
  QBUF→DQBUF ≈ 100 ms。只按画面 NAL 计
  `au_done` 会在第一帧 IDR 后把 VEPU 卡死，所以每个送达的 AU 都计数。
  `OUT_STRM_END` 后再还槽。积压的已完成 CIF 帧丢掉旧的、只编最新；spare
  空时不要再 DQ 第二帧，否则 ping-pong 只剩 1 槽、SOF 隔帧、掉到 30 fps。
  AU 队列 4。pts 用微秒。不定时强制 IDR（`RK_IDR_EVERY` 为 0），关键帧来自 GOP、
  开流、PLI 和采集重启。读者空闲（250 ms 没调 `read_packet`）才暂停 VEPU；
  采集循环的时间戳在拿锁前取，比它新的读取算活跃，不能算空闲，否则单核上
  每秒约一次丢掉队列里的 AU 并强制 IDR。
  Restart / HDMI 掉线先等 VEPU `in_flight==0` 再 STREAMOFF，
  pause 期间丢掉陈旧 AU，避免 `au_done` 对不上把新槽立刻还回去。已经
  `IN_FRM` 的包不要丢（丢掉 IDR 会让 Chrome 冻在 0 FPS）。槽满仍要 DQ，把多
  余帧立刻还 CIF，不要 `continue` 饿死采集。VEPU 慢时不要
  把未出 AU 的槽还 CIF（会覆盖正在编的帧，运动发花）。在途已满时这一帧
  还回 CIF，编下一张最新的。不要按
  `1/enc_fps` 丢 CIF 帧：1080p60 间隔刚好卡在阈值上，会把 VEPU 锁死在 ~30。
  AU 进预分配 slab，不再 `malloc+memcpy`。
  队列 4，反压与丢弃见「AU 反压与丢弃」。VBR，1080p **3 Mbps**（server remb 大约 3–5 M，
  编 5 M 会冲到 ~7.5 M）。WebRTC 必须 `read_rtcp()` 才处理 NACK/PLI；发送队列
  满了只丢 P 帧并等下一 IDR，否则 Chrome overlay 0 FPS、码率还在。发送按墙钟
  打 RTP 时间戳，不要再叠加 `prev_dropped_packets`（webrtc-rs 会再乘一次
  duration）。ICE connected 之前不要发 RTP，开流先等 IDR。1080p60 不要每 15
  帧强制 IDR（一秒 4 个 65–90 KB 关键帧，发送会掉进只出 IDR）。GOP 取 fps/2
  （`onekvm_auto_gop`，1080p60 为 30；Core 默认的 60 视为未指定），PLI 1 s 去抖；playout-delay `0,0`（Chrome 硬下限；80–160 ms 会把抖动栏钉在 ~160 ms）。kmpp `onekvm_setup` 是 High + CABAC + trans8x8，
  `qp_max` 48，**关掉 in-loop deblock**（KVM 11px 标签文字；码率和帧率不变）。
  SDP 仍是 server 的 `42e032`，不改 onekvm-server。CMA 由 DTS 固定为
  **48 MiB**（bootargs 不带 `cma=`），1080p 推流约占 35 MiB，见内核
  `drivers/kmpp/README.onekvm`。
- `encoder_unbind_source`：停流

板子装 TC358743 或 RK628F 之一：DT 两颗都描述，没装的那颗由 CIF 在 late_initcall 丢掉。
设子设备格式时沿用桥片自己的 YUV422 总线码（TC358743 `UYVY8_1X16`，RK628
`UYVY8_2X8`）；EDID 能力的 `chip_id` 按回答 DV 时序的子设备名报 `rk628` 或 `tc358743`。

HDMI 时序来自桥片 subdev 的 `VIDIOC_QUERY_DV_TIMINGS`（`ENOLINK` = 无信号）。
绑定采集每 200 ms 刷新一次并缓存；`source_input_format` / `source_signal_present`
在缓存不足 500 ms 时直接读缓存，不碰 I2C（见「读者落后」）。
两颗桥片的 EDID 都在 RAM 里，重启即丢。EDID API 的永久写入（含旧接口 `edid_set` 和
恢复出厂）先写桥片，再把副本存到 `/var/lib/onekvm/video-rk-mpp/edid.bin`；写的是出厂
EDID 时删掉副本，升级后跟随新的出厂 EDID。临时写入（`edid_set_mode` TEMPORARY）只写
桥片。每次开机的第一个 `source_create` 把副本（没有则用出厂 EDID）和桥片现有 EDID 比较，
不同才写（`VIDIOC_S_EDID`），写后等 200 ms，然后在 tmpfs 上留下
`/run/onekvm/rk-mpp-edid-restored`；此后只在桥片没有 EDID 或仍是旧 RK628 驱动内置
EDID（厂商/产品字节 `49 73 8d 62`、`24 d0 8f 62`）时补写。所以重启 `onekvm.service`
不拔插，临时 EDID 保持到设备重启。出厂 EDID 是机型包安装的
`/usr/share/onekvm/edid/presets/factory.edid.bin`，所有机型共用，源文件在 `onekvm-distro`
的 `onekvm-machine-files.inc`；RK628 内核驱动探测时加载的是逐字节相同的副本，没有副本
时开机不写。
它首选 1080p60，另含 720p60 和 640x480，CEA 扩展块声明 HDMI 接收端：Basic Audio、
一条 LPCM 2 声道**仅 48 kHz**（16/20/24 bit）的 SAD、扬声器分配（FL/FR）、HDMI VSDB
（物理地址 1.0.0.0）和 Video Capability（QS=1），并声明 YCbCr 4:4:4／4:2:2。
加了 VSDB 后，按 CTA-861 对 CE 时序（如 VIC 16）默认发限幅 RGB，QS=1 表示 AVI 的
Q 位可选全范围，是否真的发全范围或改发 YCbCr 取决于信号源驱动，需上机核对。
STREAMON 前按 JetKVM `kvm_video` 的方式等到 timings 锁住；
STREAMON 前 `S_DV_TIMINGS` **一次**；`SUBDEV_S_FMT` 不再顺带
`S_DV_TIMINGS`（会 `enable_stream(false)` 把 CSI 掐死）。
HDMI 登录/改分辨率走 `V4L2_EVENT_SOURCE_CHANGE` 和 800 ms 无有效帧：
`STREAMOFF` → `REQBUFS 0`（否则 CIF `S_FMT` queue busy）→ 先等 HDMI 掉再
等 timings 稳定约 400 ms → 再锁一次 → `S_FMT`/`REQBUFS`/`QBUF`/`STREAMON`。
判断模式是否变化时比较 timings 尺寸（`src_w`/`src_h`），不比较 `S_FMT` 结果：
RK628 把格式宽度向下取 8 的倍数（854→848、1620→1616），用格式宽度判断
时序变化会触发重复重启。
TC358743 选择 CSI lane 时要给有效像素负载留 10% 的包开销余量，并限制到板上
实际连接的 4 lane。低估 lane 数会导致截断帧和 `csi size err`，超过板上连接数会
向 CIF 报无效 lane 值。
HPD 期间用 `poll(0)` 排空 `SOURCE_CHANGE`（`DQEVENT` 空队列会阻塞）；残帧/短帧
不刷新 `last_dq`，避免假活不再重启。
WxH 变了才重开 VEPU。DQ 失败不再把 `streaming` 永久关掉。
启动后丢掉前两帧 DQBUF（TC358743 `enable_stream()` 的 LP11→HS 残帧）。
运行时 EDID 仍走同一 subdev 的 `VIDIOC_G_EDID` / `VIDIOC_S_EDID`。

对照：[LuckfoxTECH/kvm](https://github.com/LuckfoxTECH/kvm)（Go 调
`/userdata/picokvm/bin/kvm_video`）和 [jetkvm/kvm](https://github.com/jetkvm/kvm)
`internal/native/cgo/video.c`：`/dev/video0` MPLANE **YUYV**、QBUF 后再
`STREAMON`、HDMI 锁时序才开流。库存 5.10 CIF 在 CSI2 `RESETN=1` 之后就把
`CSI_DMA_ENABLE` 写进 `CTRL0`；SIZE_ERR 只 `0x000A0000` 然后 return。

## 输出缩放

输出尺寸支持 0（跟随输入）、1080/720/480 和 `宽×10000+高`：宽为 16 的倍数、
高为偶数，48×34 至 1920×1080，仅逐轴缩小且最多 8:1；不裁剪，宽高比不同时拉伸。
原尺寸仍将 CIF DMABUF 直接交给 VEPU；缩小时由独立 RGA m2m 上下文将 NV16 转为
NV12，写入一块 VEPU CMA DMABUF 再提交。该块按整 64 行补齐（宽×64 对齐高×1.5，
不小于 RGA 的 `sizeimage`），VEPU 不会越界读。在途仍为 1，CIF 4 槽、驱动 2 槽不变。

`source_reset` 先校验再改状态：尺寸不合法或当前输入缩不出时返回
`RESOURCE_UNSUPPORTED` 并写明原因，配置和采集都不变；不重写 EDID、不拉 HPD。
后端声明 `SOURCE_RESET_WHILE_BOUND`（bit 17），Core 在解绑前先调用它：被拒绝时直播
不中断，配置接口返回 400 和该原因、不回滚；接受的尺寸在重新绑定、打开 VEPU 时生效。

打开编码器时若输入已缩不出（HDMI 换了模式）、RGA 不可用或缩放块分配失败，记录
原因并按原尺寸编码，绑定不失败。运行中 RGA 任务失败只丢该帧并重开 RGA 上下文；
连续 3 次或重开失败，本次绑定改为原尺寸（重启采集并重开 VEPU），重新绑定才再试
缩放。缩放通道绝不提交未缩放的 CIF 帧。采集线程自行退出后，再次 bind 会重启它，
不会把已停的线程当作仍在绑定。

## Rockchip kmpp

RV1106 编码是内核 **kmpp**（`/dev/vcodec` + VEPU540C），不是上游
`rockchip-linux/mpp`。默认路径：`ALLOC_DMA` 的 VEPU CMA 做 CIF
`V4L2_MEMORY_DMABUF`，ioctl `VCODEC_CHAN_IN_FRM_RDY` /
`OUT_STRM_BUF_RDY`。拿不到 fd 再回退 MMAP+`EXPBUF`。不链 `librockit`。
`strm_pool` 在第一帧 encode 时才建；`buf_pool_get_free_num(NULL)` 必须当
「还可以编」，否则 CIF 有帧 VEPU 也不 `hw_start`。

热路径没有用户态帧拷贝：CIF DMABUF（或 RGA 输出）直接 `IN_FRM`，AU 进 4 槽 slab。
内核 `GetStream` 仍会把 strm_pool 环 `copy_to_user` 到 slab（绕回时打
`stream wrap copy`）；要消掉得 mmap kmpp 码流环（内核），现在没做。
Core 经 `encoder_read_packet` 借出 AU，复制后即释放。不导出 `encoder_take_packet`：
Core 的 `EncodedAuRing` 有 64 格，会长期持有转移出去的 slab，4 槽很快占满，直播冻结。
无信号画面只在变化时重绘进同一块 VEPU DMA 缓冲，其余帧直接重复提交。

### AU 反压与丢弃

4 槽全占时 drain 最多等 16 ms（释放槽立即唤醒）后返回重试：AU 留在内核，在途仍为 1，
采集在 `IN_FRM` 前跳过 CIF 帧，码流保持可解、帧率下降。不扩池，也不抢已排队 AU 的
槽——抢槽会让后续 P 帧缺参考（H.264 frame_num／H.265 POC 断档、灰屏）。

不交付依赖已丢 AU 的帧：队列溢出时丢最老一帧及其后直到下一关键帧的非关键 AU；
队列里没有关键帧时，后续非关键 AU 一律丢弃并强制 IDR，直到关键帧到达。关键帧不按
此规则丢弃，新到的是关键帧时不另行强制 IDR。读者空闲、采集重启、失锁等主动清队列
同样等待强制 IDR。已 `IN_FRM` 的帧仍照常取出、计入 `au_done`，只在出队后丢弃。

每 2 s 的 `rk-mpp: dq=… done=…` 行末尾：`qdrop=` 为按上述规则丢弃的 AU 累计数，
`slabwait=` 为等槽次数，unbind 时清零。持续增长说明读者跟不上。

### 读者落后

WebSocket 路径上 Core 每发一个 AU 调两次 `source_input_format`。旧实现每次都在 source
锁内 `QUERY_DV_TIMINGS`，涉及 I2C 寄存器和 sysfs 读取，出队可能因等待这些
操作而滞后。两个状态接口读取采集线程的时序缓存，以满足 ABI 的非阻塞要求。

`ONEKVM_VIDEO_FEATURE_LATENCY`：`source_latency` / `encoder_latency` 只读缓存
纳秒。绑定热路径记 CIF FS→DQ（采集，约 1 帧）和
`IN_FRM_RDY→OUT_STRM`（编码，在途 1 帧）。status/SSE 不得 `DQBUF` /
`GetStream`。`source_read` 回退路径记 `poll+DQBUF`。

`ONEKVM_VIDEO_FEATURE_BOUND_ENCODED_TIME`：绑定 AU 的 `pts_ns` 是取到编码输出
（`OUT_STRM_BUF_RDY` / `GetStream` 返回）时的 `CLOCK_MONOTONIC`，随 AU 穿过队列；
不用 `pkt.u64pts`（输入帧时间）。Core 据此发送网页“编码到收帧”。

`ONEKVM_VIDEO_FEATURE_ENCODER_COLOR_INFO`（仅 kmpp 构建）：报最近交出的关键帧
SPS 所标的 8 bit 与范围，交出第一个关键帧前报未知。打开 VEPU 时用
`VIDIOC_SUBDEV_G_FMT` 读桥片格式：TC358743 UYVY 只报 `SMPTE170M`，按 V4L2 默认推出
BT.601、有限范围，经 `onekvm_setup` 的 `range`/`matrix` 写进 VUI；RK628F 的 CSC
总是输出全范围 YCbCr，驱动按输出报 `REC709`（BT.2020 输入时报 `BT2020`）、全范围，
VUI 标全范围和 BT.709 矩阵（BT.2020 不标矩阵）。原色和传递特性不标。kmpp 只在全范围或有矩阵时写 `video_signal_type`，所以有限范围要连同矩阵下发，
没有矩阵时报 `UNSPECIFIED`。读不到格式、或内核只收 `KMPP_ONEKVM_SETUP_V1_SIZE` 的短
结构时，同样不标并报 `UNSPECIFIED`。无信号通道按缓存的 BT.601 有限范围下发，报
`LIMITED`；每个 AU 记录入队时所属通道的范围，切换通道不会改标已排队的 AU。快照 JPEG
没有 VUI，始终用短结构。Core 的
`video.color_range` 配置目前不改码流。

### 无信号画面

Core 的 `onekvm-no-signal-cache` 把当前语言（或自定义图片）的无信号画面写成
`/data/onekvm/cache/no-signal/current/<宽>x<高>.nv21`（BT.601 有限范围；1920×1080、
1280×720、640×480 三种）。环境变量 `ONEKVM_NO_SIGNAL_CACHE` 可替换缓存根目录。采集停止
时捕获线程关掉采集用的 VEPU 通道，改开无信号通道：

- 尺寸取 Core 请求的输出尺寸，没有请求时取最后的采集尺寸。读最接近的缓存文件，等比
  缩放居中、四周补黑，NV21 转 NV12 画进一块 VEPU DMA 缓冲；文件缺失或大小不符时画黑帧。
- 通道 10 fps、GOP 20、码率与实况相同；每 100 ms 提交同一缓冲。读者空闲 250 ms 后停交，
  读者回来先出 IDR。
- 每秒 `stat` 一次当前文件（设备号、inode、mtime），变了就重绘并出 IDR，切换语言或
  自定义图片在 1 s 内生效。
- 采集恢复时 `capture_restart_stream` 关掉无信号通道，按采集尺寸重开 VEPU。
- 通道打开失败每秒重试一次；VEPU 1 s 未交回上一帧按丢失处理，不会卡住。
- `capture_restart_stream` 等待时序期间，继续发送无信号画面。

## JPEG 单次快照

`source_snapshot` 使用独立 kmpp MJPEG 通道（coding=8、`max_strm_cnt=1`、禁用共享
缓冲），不从 H.264/H.265 码流取图，不解绑或重启直播。内核允许多编码通道：
`mpp_vcodec_chan_create` 为已占用通道另选空闲号，JPEG 有独立的 VEPU540C HAL。
两者仍共用 VEPU 调度，不能据此保证直播延迟不变，必须实测硬件竞争和 CMA 余量。

同一时间仅接纳一个快照。采集线程只发布一帧 CIF DMABUF；快照调用线程执行独立
RGA NV16→NV12 和 JPEG。CIF 槽要同时等直播 AU 完成和快照 RGA 完成才能再次 QBUF；
不新增 CIF 槽，不做 CPU 图像拷贝。原尺寸快照也经过 RGA 等尺寸转换，以便尽早归还
CIF 槽，JPEG 只读独立 CMA 输出块。无直播时临时启动同一绑定采集路径，不送 H.26x
输入帧，结束即释放；已有采集的所有权不能被抢走。源切换／停止等待 RGA 归还输入。
冷启动采集要等 HDMI 时序锁定，默认超时可能不够；Core 调用时传 1000 ms。

快照尺寸沿用缩放对齐和比例限制；0×0 取配置输出尺寸（原尺寸配置则跟随输入）。
`quality_factor=0` 用 80，
其余映射到 JPEG 1–99 并固定上下限。配置使用 RV1106 32 位 `MppEncCodecCfg`
布局（2832 字节、JPEG 偏移 4；其余联合体填零、无量化表指针）。内核 SET_CODEC_CFG
会复制完整联合体，不能只传 JPEG 短前缀；不将内核 `MppEncCfg` 指针传入用户态。
默认超时 250 ms、上限 1000 ms，采集等待、RGA 和 JPEG poll 共用单调时钟截止点；
超时后先 STREAMOFF／关闭独立硬件上下文再归还输入，驱动清理耗时不受 poll 截止点保证。
容量不足返回 `BUFFER_TOO_SMALL` 和实际字节数，允许空指针＋零容量探测，不写越界，
所有已取得 JPEG 输出均调用 `OUT_STRM_END`。Core 的流式 MJPEG 路径不调用此接口。

快照的 CMA 开销包括 NV12 输出块（按 64 行对齐）与 JPEG 码流池。分配失败时
结束该次快照，直播继续运行。

显示会话（RGA m2m）的 OUTPUT 队列以 DMABUF 导入 CIF 缓冲。vb2 在 DQBUF 之后仍挂着
最后一次 QBUF 的 DMABUF，直到 REQBUFS(0) 或换另一块，所以采集停止、释放 CIF 缓冲前
要先调用 `rk_source_release_display`：否则最后一块 CIF 缓冲（1080p 4 MiB）在空闲快照
或推流结束后一直占着。下一帧到来时显示会话重新建立。

显示的 YCbCr→RGB 按桥片报的矩阵和范围换算（与编码 VUI 同源，最多 1 s 读一次子设备格式，
显示会话释放后重读）：TC358743 为 BT.601 有限范围，RK628F 为 BT.709 全范围。RGA
OUTPUT 的 S_FMT 带 `ycbcr_enc`/`quantization`，内核 RGA 驱动据此选 CSC 模式；硬件 CSC
字段没有全范围 BT.709，全范围一律用 BT.601 全范围矩阵（灰阶准确，饱和色有轻微色相差）。
不认这两个字段的旧内核按 colorspace 走有限范围，与以前相同。A7 回退按同样的矩阵和范围换算，
不近似。读不到子设备格式时沿用旧做法（RGA 按高度猜 BT.601/709，A7 用 BT.601，均为有限范围）。

监测驱动占用时，查看 `/sys/devices/platform/onekvm-lazy-cma/stats` 的
`in_use_bytes` 和 `/sys/kernel/debug/dma_buf/bufinfo`。CMA 页块也可承载可迁移
的页缓存，释放的页还可能暂留在每 CPU 空闲链表。

CMake `-DONEKVM_HAVE_ROCKIT=ON` 仍是可选的 uclibc MPI 路径，OE 默认关闭，不出无信号
画面。

## 构建

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

设备 IPK：

```sh
cd onekvm-distro
ONEKVM_MACHINE=onekvm-picokvm \
ONEKVM_DEBUG_WORKTREES=onekvm-video-rk-mpp \
    make package onekvm-video-rk-mpp
```
