## 项目简介

本项目基于 i.MX6ULL Linux 平台，实现 OV5640 摄像头采集、LCD 实时显示和本地 BMP 图片保存。
设备树和内核驱动负责 I2C、CSI 及摄像头初始化，用户程序通过 V4L2 设备获取视频帧。

## 功能

- 使用 V4L2 MMAP 方式采集图像。
- 默认请求 640x480、YUYV 格式，实际参数以驱动返回值为准。
- 支持 YUYV、UYVY 和 RGB565 三种摄像头返回格式。
- 使用 BT.601 整数公式完成 YUV 转 RGB。
- 根据 framebuffer 返回的 RGB 位域写入 LCD，不把像素布局写死。
- 使用最近邻方法将摄像头画面缩放并铺满 LCD，允许轻微比例变形。
- 双页 framebuffer 可用时，使用 FBIOPAN_DISPLAY 在 VSYNC 时切换页面。
- 没有双页显存或页面切换失败时，退回后备缓冲区和 FBIO_WAITFORVSYNC 复制模式。
- 每小时保存一张最新画面，按下并释放 KEY0 后立即保存一张画面。
- 图片保存为 640x480、24 位 BMP，文件名精确到毫秒。
- 支持 Ctrl-C 和 SIGTERM 退出，并统一释放线程、视频流和 LCD 资源。

## 目录结构

~~~text
├── README.md                       当前说明文件
├── docs/                           实际运行记录
│   ├── dmesg-camera.txt            摄像头、CSI 和 LCD 内核日志
│   ├── fbset-info.txt              LCD framebuffer 参数
│   ├── runtime-log.txt             程序启动和保存图片日志
│   └── v4l2-info.txt               V4L2 设备节点和识别记录
└── src/                            正式源代码
    ├── Makefile                    用户程序和内核模块编译入口
    ├── camera.c                    主函数、取帧循环和退出流程
    ├── camera.h                    公共结构体、路径和接口
    ├── camera_capture.c            V4L2 采集、格式转换和 LCD 显示
    ├── camera_save.c               BMP 保存、定时事件和按键线程
    ├── camera_timer.c              一小时 timer_list 字符设备模块
    ├── camera_key.c                KEY0 GPIO 中断字符设备模块
    └── camera_timer_events.h       事件定义
~~~

## 数据流程

~~~text
OV5640
  -> I2C 配置和 CSI 连接
  -> MXC V4L2 驱动
  -> /dev/video0 或 /dev/video1
  -> V4L2 MMAP 缓冲区
  -> YUV/RGB 转换
  -> 缩放到 LCD 分辨率
  -> /dev/fb0 页面切换
~~~

定时保存和按键保存流程：

~~~text
timer_list 到期 -> /dev/camera_timer 的 T 事件
KEY0 中断      -> /dev/key -> 用户线程写入 K 事件
T/K 事件       -> 保存线程复制最新帧 -> 生成 BMP
~~~

定时器回调和 GPIO 中断只产生事件、唤醒等待队列，不在内核中执行文件操作。保存线程没有
事件时阻塞在 read()，不使用每秒轮询。

## 源码说明

### src/camera.c

负责注册信号、打开设备、启动视频流和保存线程，并循环执行：

~~~text
select() -> VIDIOC_DQBUF -> 显示 -> 复制最新帧 -> VIDIOC_QBUF
~~~

程序参数格式：

~~~text
camera [video-device] [framebuffer-device] [save-directory] [key-device]
~~~

默认值定义在 src/camera.h：

~~~text
video-device:       /dev/video0
framebuffer-device: /dev/fb0
save-directory:     /home/lzy/linux/imx6ull/Drivers/Linux_Drivers/camera/data
key-device:         /dev/key
timer-device:       /dev/camera_timer
~~~

当前代码中定时器路径使用 DEFAULT_TIMER_DEVICE，不作为命令行参数传入。

### src/camera_capture.c

- 使用 VIDIOC_QUERYCAP 检查视频采集和流式传输能力。
- 使用 VIDIOC_S_FMT 请求 640x480 YUYV。
- 使用 VIDIOC_REQBUFS 申请 4 个 MMAP 缓冲区。
- 读取 LCD 的分辨率、色深、行跨度和 RGB 位域。
- 每个摄像头像素只转换一次，缩放时直接复制 LCD 像素。
- 双缓冲时绘制非显示页，再调用 FBIOPAN_DISPLAY 切页。
- 页面切换失败时打印错误并退回复制模式。

### src/camera_save.c

保存线程阻塞读取 /dev/camera_timer，收到 T 或 K 事件后，在互斥锁保护下复制
latest_frame，手动写入 24 位 BMP。BMP 每行按 4 字节对齐，并从底行开始写入。

KEY0 线程阻塞读取 /dev/key，收到按键释放事件后向定时器设备写入 K。文件名包含年月日、
时分秒和毫秒，例如：

~~~text
camera_20261007_112730_393.bmp
~~~

### src/camera_timer.c

这是一个内核字符设备模块，设备名为 /dev/camera_timer：

- 使用 timer_list 每 3600 秒产生一次 T 事件。
- 使用等待队列让用户线程阻塞等待。
- write() 接收 K 或 Q 事件并唤醒等待线程。
- 卸载模块时使用 del_timer_sync() 删除定时器。

### src/camera_key.c

这是本项目的 KEY0 GPIO 平台驱动：

- 通过 compatible = "lzy,camera-key" 与设备树匹配。
- 通过设备树的 key_gpio 属性获取 GPIO，不写死 GPIO 编号。
- 使用上升沿和下降沿中断，20 ms 定时器完成去抖。
- 按键按下后再释放时，向 /dev/key 提供值为 1 的事件。

## 设备树要求

本仓库不包含也不自动修改设备树。实际设备树需要确认：

- OV5640 位于实际连接的 I2C 控制器下。
- compatible、reg = <0x3c> 和 status = "okay" 正确。
- PWDN、RESET、24 MHz MCLK 和 GPIO 有效电平与底板一致。
- OV5640 endpoint 与 CSI endpoint 正确连接。
- KEY0 使用 compatible = "lzy,camera-key"，并提供 key_gpio 和中断属性。
- LCD timing 与实际屏幕一致，本项目使用 1024x600。

修改设备树后，需要重新编译 DTB、替换开发板实际启动的 DTB 并重启。

## LCD 双缓冲和偶发撕裂

用户空间后备缓冲区只能减少逐像素刷新，不能保证完全消除撕裂。要使用页面切换，需要
内核 mxsfb 提供两页 framebuffer。

## 问题定位与优化过程

### 摄像头无法识别

初始加载摄像头模块时曾出现 camera ov5640 is not found。排查时先确认 I2C 总线和地址，
通过 i2cdetect 发现摄像头在 i2c-1 的 0x3c 响应，随后内核日志出现 ov5640 1-003c 和
camera ov5640, is found。这个过程说明设备树中的 I2C 控制器不能只根据节点名称判断，
还需要和实际硬件连接及 Linux 运行时编号对应。

### LCD 全屏显示和颜色处理

摄像头输出为 640x480，LCD 为 1024x600，直接显示会留下空白区域，因此增加最近邻缩放并
铺满 LCD。两个分辨率的宽高比不同，铺满显示会有轻微变形。

早期 YUV 转换没有处理限定范围，存在亮度和颜色偏差。当前使用 BT.601 整数公式，将 Y
减去 16、U/V 减去 128，再根据 framebuffer 返回的 RGB 位域写入 16 位 RGB565。实际参数
为 bpp=16、stride=2048、RGB=(11/5,5/6,0/5)。

### 刷新慢、重影和横向撕裂

早期版本直接逐像素写 framebuffer，刷新较慢并出现重影。随后增加用户空间后备缓冲区，先
完整生成一帧，再使用 FBIO_WAITFORVSYNC 和整帧复制，重影有所改善，但单页显存仍可能在
LCD 扫描期间被修改，所以长时间运行仍会出现横向割裂。

最终修改 mxsfb，使 yres_virtual 至少为 yres 的两倍；用户程序把下一帧写入非显示页，再
调用 FBIOPAN_DISPLAY 让 LCD 控制器在 VSYNC 时切换。当前日志已经确认 virtual=1024x1200
和 framebuffer pages=2，大部分时间显示正常，但偶发撕裂仍可能与旧版驱动时序、FIFO 错误、
其他 framebuffer 写入者、帧率不同步或 LCD timing 不匹配有关。

### 定时保存和资源释放

不采用每秒轮询，而是使用 timer_list、等待队列和字符设备事件。保存线程平时阻塞等待，
一小时事件或 KEY0 事件到来时才保存 BMP。退出时先唤醒并等待线程，再停止视频流、解除
mmap、释放缓冲区和关闭设备。

文件名从秒精度改为毫秒精度，避免短时间连续按键产生同名文件。网络上传功能最终移除，
项目范围保持为摄像头采集、LCD 显示和本地 BMP 保存。

## 实测记录

实际记录保存在 docs/：

- dmesg-camera.txt：出现 ov5640 1-003c 和 camera ov5640, is found。
- v4l2-info.txt：实际测试摄像头节点为 /dev/video1，设备名为 mx6s-csi。
- fbset-info.txt：LCD 为 1024x600、约 60 Hz、16 bpp、虚拟分辨率 1024x1200，行跨度
  2048，RGB565 位域为红 5/11、绿 6/5、蓝 5/0。
- runtime-log.txt：程序检测到双缓冲，并成功保存带毫秒文件名的 BMP。
