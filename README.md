# 工控机 OAK 相机 HTTP 服务

独立 C++17/DepthAI/OpenCV/FFmpeg 服务，在 Ubuntu/Jetson 提供预览、照片、录像及媒体参数 HTTP API。与 `Tenco/`、`src/` 并列，不链接到 Windows Qt 应用，不作为 ROS 包构建。厂商 SDK 使用目标机 `/opt/ros/humble` 已有安装。

完整工作空间的唯一约束为 [根 AGENTS.md](../AGENTS.md)，版本、最近部署与未解决问题集中在 [项目说明](../docs/项目总览.md)。本仓库只保存服务源码、测试、脚本、systemd 和必要接口说明。

## 构建与测试

Ubuntu 22.04/Jetson ARM64 已有 DepthAI 2.31.1、OpenCV 4.8；另需 pkg-config、JSONCPP、libavformat/libavcodec/libavutil 开发库及带 libx264 的 FFmpeg，测试需要 ffmpeg/ffprobe。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=/opt/ros/humble
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

恢复测试运行生产恢复循环，覆盖设备缺失、读帧异常、卡帧、缓存、停止/重开与录像不自动续录。HTTP 测试替换为假采集源，覆盖多帧、503/EOF、重连、媒体参数和客户连接回收；不占用 USB、不修改在线服务。物理 USB、SDK 卡死和长期故障另需现场验收。

## 接口与输出

完整字段、并发、错误、持久化、curl/Python 调用见 [相机媒体参数与拍摄接口调用说明](docs/相机媒体参数与拍摄接口调用说明.md)。

| 方法与路径 | 用途 |
|---|---|
| GET /camera/capabilities | 原生模式、尺寸/FPS 和编码能力 |
| GET/PUT /camera/settings | 版本校验、配置写入和实际应用状态 |
| GET /camera/status | 采集健康、实际参数、帧龄、录像/丢帧指标 |
| GET /camera/stream.mjpeg | MJPEG 预览 |
| POST /camera/photo | 保存一张 JPEG |
| POST /camera/record/start、/stop | 开始录像、停止并封装 MP4 |

默认 IMX378 原生输入 4056×3040@30；预览 960×540@15/JPEG70；照片原生 JPEG90/源 2 FPS；录像 1280×720@30 目标/H.264 yuv420p MP4。预览/照片/录像可独立调整，比例不符时保留完整视野补黑边。照片 FPS 指源刷新，不代表自动批量保存。原生 12MP 录像可选但不保证 15 FPS，默认低分辨率档位按真实文件 FPS 和完整解码验收。

PUT 使用 expectedRevision，原子持久化后返回 202，GET 区分 pending/applied/rejected 和 activeSettings。采集线程重建应用配置，HTTP 不等待 SDK 打开/销毁；失败恢复上一有效配置，应用中退出也保留 fallback。录像中或未就绪拒绝改参，客户端不能在启动或本地保存时覆盖设备设置。

原生/照片/录像/预览的健康按合法新帧和单调时钟判断；默认连续 5 秒无帧重建，正常帧新鲜度默认 1 秒，低 FPS 按至少 1.5 个输出周期放宽。失败退避 1/2/4/8/10 秒，健康 5 秒复位。未连接/旧缓存流返回 503；采集故障关闭录像，恢复后不自动续录。

录像使用真实时间戳，不填重复帧；编码队列最多 3 帧，满时丢最旧帧并统计，默认 CRF23/4 线程。状态和预览不等待磁盘封装；正常停止等待有界队列并封装，分片降低异常退出影响，不承诺断电零丢失。

status 保留 frameId、lastFrameAgeMs、reconnectCount、lastCaptureError、原生帧龄和尺寸、recordingActualFps/recordingDroppedFrames/lastRecordingError、mediaConfiguration 等诊断。previewJpegOptimized 表示预览 Huffman 优化，默认开启，可用 `--disable-preview-jpeg-optimization` 关闭；不改变照片/录像或 settings revision。同输入编码历史减少约 4%，不是所有无线场景带宽收益的承诺。

DepthAI 2.31.1 的 XLinkOut::setFpsLimit 在该设备曾导致停流，当前使用设备 Script 选完整帧后缩放/传输。SDK 打开/销毁使用自身超时，不能保证物理设备/SDK 卡死时具有假源相同停止时延。最多 16 客户，socket 读写超时 2 秒，退出中断客户并回收线程。更多参数见 `--help`，非法数值/关系明确退出。

## 运维和部署

2026-10-17 已统一部署至 `/home/nvidia/tenco-project`：源码为 `oak-camera-service/`，构建为 `build/camera/`，在线程序为 `install/camera/oak_camera_service`，systemd 为 `tenco-oak-camera.service`。照片/录像保存至源码目录的 `Capture/`；旧 `/opt/tenco/media` 的历史媒体移至 `Capture/legacy/` 并保留旧路径兼容链接，保护全部业务媒体。`Capture/`、构建产物和凭据不纳入版本管理。

目标机完整服务文件位于项目 `systemd/tenco-oak-camera.service`，由 /etc/systemd/system 链接加载，不再使用历史宽泛 pkill。仓库的 `systemd/20-native-capture.conf` 已同步新安装及媒体路径，可供媒体参数升级事务使用。上线前核对 `systemctl cat tenco-oak-camera.service` 和实际 ExecStart，不从本地文件推断现场版本。在项目根目录可使用 `bash src/deployment/project.sh build-camera`、`test`、`status`；构建不会覆盖 install/ 内的在线程序，部署脚本负责已验证候选的安装切换。

用户明确要求部署后，在已授权维护窗口使用已验证产物及 SHA256：

- `scripts/deploy_verified_service.sh VERIFIED_BINARY SHA256`：拒绝录像中切换，临时保留旧程序、原子安装并重启；连续健康/新帧和哈希验证后删除临时旧程序，失败先恢复。
- 媒体参数升级使用 `scripts/deploy_media_settings.py VERIFIED_BINARY SHA256 --drop-in systemd/20-native-capture.conf --evidence /绝对路径/evidence.json`，以 root 执行；同时验证原生照片、默认录像真实 >=15 FPS、完整解码和配置回退。

旧脚本的持续新帧检查不能替代媒体参数升级验收。只保留当前有效版本，不保留长期重复二进制；不能使用已删除的旧备份回退，必要时从受控源码重建。部署不扩展到车辆/云台运动、USB 故障注入、删除业务媒体或修改其他进程。
