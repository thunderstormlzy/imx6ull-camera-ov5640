# OV5640 Camera Capture for i.MX6ULL

## 项目简介

本项目基于 i.MX6ULL Linux 平台，实现 OV5640 摄像头采集、LCD 实时显示和本地图片保存。
摄像头通过 I2C 完成设备识别和初始化，通过 CSI 与 MXC V4L2 驱动输出视频帧。

项目不包含网络上传功能，图片仅保存在开发板本地文件系统中。

## 主要功能

- 使用 V4L2 MMAP 方式采集 OV5640 图像。
- 默认采集分辨率为 `640x480`，像素格式为 `YUYV`。
- 将摄像头图像转换后显示到 LCD framebuffer。
- 将摄像头画面缩放并铺满 LCD；由于摄像头和 LCD 比例不同，画面可能有轻微变形。
- 内核提供两页 framebuffer 时，把下一帧写入非显示页，再用 `FBIOPAN_DISPLAY` 在 VSYNC 时切换，
  从根本上避免单页写入造成的横向撕裂。
- 如果开发板仍只有一页虚拟显存，程序退回“后备缓冲区 + `FBIO_WAITFORVSYNC` + 复制”的兼容路径，
  但该路径不能完全保证不撕裂。
- 每小时自动保存一张最新图像。
- 按下并释放 KEY0 后立即保存一张最新图像。
- 图片保存为 24 位 BMP 格式，文件名包含毫秒时间。
- 支持 Ctrl-C 退出，并统一释放线程、摄像头和 LCD 资源。

## 软件结构

```text
src/
├── camera.c                  用户程序入口和采集主循环
├── camera_capture.c          V4L2 摄像头和 LCD 显示
├── camera_save.c             BMP 保存、定时事件和 KEY0 处理
├── camera_timer.c            内核一小时定时器模块
├── camera_key.c              KEY0 GPIO 中断模块
├── camera.h                  公共数据结构和接口
├── camera_timer_events.h     定时器与用户程序共用的事件定义
└── Makefile                  用户程序和内核模块编译文件
```

## 数据流程

```text
OV5640
  -> I2C 配置
  -> CSI
  -> MXC V4L2 驱动
  -> /dev/video*
  -> V4L2 MMAP 缓冲区
  -> 用户程序
  -> LCD framebuffer /dev/fb0
```

定时保存和按键保存共用同一条流程：

```text
timer_list 或 KEY0 中断
  -> /dev/camera_timer 事件
  -> 用户保存线程被唤醒
  -> 复制最新帧
  -> 生成 BMP 文件
```

定时器回调只负责产生事件和唤醒等待队列，不在内核定时器上下文中执行文件操作。

## 问题定位与优化过程

### 1. 摄像头无法识别

最初加载摄像头驱动时出现 `camera ov5640 is not found`。排查时发现不能只看设备树
节点名称，还要确认运行时的 Linux I2C 编号。通过 `i2cdetect` 检查后，摄像头在
`i2c-1` 的 `0x3c` 地址响应，最终日志为 `ov5640 1-003c` 和 `camera ov5640, is found`。
这说明 I2C、供电、复位、时钟和 CSI 链路已经匹配。

### 2. 摄像头和 LCD 分辨率不同

摄像头输出为 `640x480`，LCD 为 `1024x600`。如果直接按原尺寸显示，会出现空白区域；
如果逐点计算缩放，会增加 CPU 负担。当前实现先把每个摄像头像素转换为 LCD 像素格式，
再使用最近邻方法铺满 `1024x600`。因为两个宽高比不同，画面允许轻微变形，但 LCD 没有黑边。

### 3. 刷新慢、重影和横向割裂

早期版本直接逐像素写 framebuffer，导致刷新慢；随后增加用户空间后备缓冲区，先完整生成
一帧，再复制到 framebuffer，并使用 `FBIO_WAITFORVSYNC`，改善了重影，但单页显存仍可能在
LCD 扫描期间被修改，因此长时间运行后仍能看到横向割裂。

最终方案分为两部分：

1. `camera_capture.c` 检测 framebuffer 是否至少有两页显存。
2. 将下一帧写入非显示页，调用 `FBIOPAN_DISPLAY`，由 `mxsfb` 在 VSYNC 时切换页面。

内核 `mxsfb.c` 将 `yres_virtual` 保证为 `2 * yres`，使 `1024x600` LCD 实际得到
`1024x1200` 虚拟分辨率。实测程序日志显示 `framebuffer pages=2`，说明双缓冲已启用。
如果运行环境没有双页显存，程序会退回兼容复制模式，但该模式不能完全消除撕裂。

### 当前实际情况

当前开发板日志已经确认：

```text
virtual=1024x1200, RGB=(11/5,5/6,0/5), framebuffer pages=2
```

大部分时间画面正常，说明双页 framebuffer 和页面切换路径已经生效；但仍偶发横向撕裂，
因此当前结果应描述为“明显改善，但还没有做到百分之百无撕裂”，不能简单认为问题已经
完全解决。

### 4. 颜色失真

显示链路中摄像头输出 YUYV，而 LCD framebuffer 使用 RGB565。早期转换公式没有处理 YUV
限定范围，可能造成亮度和颜色偏差。当前使用 BT.601 整数转换公式，将 Y 减去 16、U/V
减去 128，再根据 framebuffer 返回的 RGB 位域打包像素；不把 RGB565 写死在用户程序中。

当前实际 LCD 参数记录如下：

```text
camera: 640x480 -> LCD 1024x600
bpp=16, stride=2048, virtual=1024x1200
RGB=(11/5,5/6,0/5), framebuffer pages=2
```

### 5. 定时保存和资源问题

不采用每秒轮询检测，而是使用内核 `timer_list`、等待队列和字符设备事件。保存线程平时
阻塞等待，只有一小时定时事件或 KEY0 事件到来时才保存 BMP。程序退出时设置停止标志、
唤醒阻塞线程、等待线程结束，再停止视频流、解除映射并关闭设备，避免线程和设备资源泄漏。

文件名从秒精度改为毫秒精度，解决短时间连续按键时文件名重复覆盖的问题。网络上传功能
最终移除，项目范围保持为摄像头采集、LCD 显示和本地 BMP 保存。

## 图片文件

图片默认保存到：

```text
/lib/modules/4.1.15+/camera/data
```

文件名格式：

```text
camera_YYYYMMDD_HHMMSS_milliseconds.bmp
```

## 设备树要求

本项目不直接修改设备树。使用前应确认设备树中包含以下配置：

- OV5640 节点位于实际连接的 I2C 控制器下。
- 摄像头地址为 `0x3c`，设备状态为 `okay`。
- OV5640 与 CSI endpoint 正确连接。
- PWDN、RESET 和 MCLK 配置与硬件连接一致。
- KEY0 节点与 `camera_key.ko` 的 `compatible` 和 GPIO 属性匹配。

设备树修改后，需要重新编译 DTB、替换开发板启动使用的 DTB 并重启。

## LCD 双缓冲前提

为让 `FBIOPAN_DISPLAY` 有第二页可切换，需要修改内核文件
`drivers/video/fbdev/mxsfb.c` 的 `mxsfb_check_var()`，将 `yres_virtual` 至少设为
`2 * yres`，然后重新编译并更新开发板内核。设备树不需要因为双缓冲而修改。