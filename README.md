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
| GET /camera/depth/status | 双目支持、帧龄、传输状态与毫米单位说明 |
| GET /camera/depth/image.png | 无损 uint16 毫米深度图，0 为无效像素 |
| GET /camera/depth/image.jpg | 便于显示的伪彩色深度图 |
| GET /camera/depth/stream.mjpeg | 实时伪彩色深度 MJPEG 流 |
| POST /camera/photo | 保存一张 JPEG |
| POST /camera/record/start、/stop | 开始录像、停止并封装 MP4 |

默认 IMX378 原生输入 4056×3040@30；预览 960×540@15/JPEG70；照片原生 JPEG90/源 2 FPS；录像 1280×720@30 目标/H.264 yuv420p MP4。预览/照片/录像可独立调整，比例不符时保留完整视野补黑边。照片 FPS 指源刷新，不代表自动批量保存。原生 12MP 录像可选但不保证 15 FPS，默认低分辨率档位按真实文件 FPS 和完整解码验收。

PUT 使用 expectedRevision，原子持久化后返回 202，GET 区分 pending/applied/rejected 和 activeSettings。采集线程重建应用配置，HTTP 不等待 SDK 打开/销毁；失败恢复上一有效配置，应用中退出也保留 fallback。录像中或未就绪拒绝改参，客户端不能在启动或本地保存时覆盖设备设置。

RGB 预览、原生照片和录像在设备端按需开启：预览订阅续期 2 秒，照片请求等待请求之后的新原生帧，录像先准备编码器再开启传输，首帧提交后返回成功。启动或改参时三路各验证新帧，此后空闲只以最高 2 Hz 传输真实 ISP 帧元数据（无像素），不缩放图像、不做 JPEG 编码；状态查询不会隐式启动视频。`cameraConnected` 按该健康流新鲜度判断，`captureFrameId` / `lastCaptureFrameAgeMs` / `captureHealthFps` 供远程启动持续验证，既有 `frameId` / `lastFrameAgeMs` 仍专指预览。

仅对已请求的照片/录像/预览检查对应断流；健康流持续检查。默认连续 5 秒无帧重建，正常帧新鲜度默认 1 秒，低 FPS 按至少 1.5 个输出周期放宽。失败退避 1/2/4/8/10 秒，健康 5 秒复位。未连接/旧缓存流返回 503；采集故障关闭录像，恢复后不自动续录。没有订阅者时预览帧号停止递增属于正常空闲，不应据此重启相机。

设置 `persisted=true` 仅在文件和父目录同步均确认后成立。rename 后目录 open/fsync 失败仍跟随已经可见的新 revision 应用及恢复 fallback，但返回 `persistenceState=unconfirmed`、`persisted=false` 和 `persistenceError`，不把运行生效宣称为断电持久化已确认；未设置配置文件时为 `memory_only`。

录像使用真实时间戳，不填重复帧；编码队列最多 3 帧，满时丢最旧帧并统计，默认 CRF23/4 线程。状态和预览不等待磁盘封装；正常停止等待有界队列并封装，分片降低异常退出影响，不承诺断电零丢失。

status 保留 frameId、lastFrameAgeMs、reconnectCount、lastCaptureError、原生帧龄和尺寸、recordingActualFps/recordingDroppedFrames/lastRecordingError、mediaConfiguration 等诊断。previewJpegOptimized 表示预览 Huffman 优化，默认开启，可用 `--disable-preview-jpeg-optimization` 关闭；不改变照片/录像或 settings revision。同输入编码历史减少约 4%，不是所有无线场景带宽收益的承诺。

深度使用 CAM_B/C 上真实单目传感器的 StereoDepth，默认 640×400@10 FPS，参考为校正后的左相机，**未对齐 RGB**。PNG 解码须保留 16 位（OpenCV 使用 `IMREAD_UNCHANGED`），每像素数值为毫米，0 无效；JPEG/MJPEG 是可视化，不能从其颜色当作精确距离。默认可视化范围 300～10000 mm，范围外颜色截断，PNG 原值不裁剪。设备缺少双目、显式禁用或无新鲜深度帧时图像接口返回 503，绝不由 RGB 生成假深度。`capabilities.depth` 与 `status.depth` 同时提供状态。启动选项为 `--disable-depth`、`--depth-fps 1..15`、`--depth-jpeg-quality 1..90`、`--depth-visual-min-mm`、`--depth-visual-max-mm`，与既有 RGB settings revision 分开，需重启服务生效。

深度只在图像/流请求后申请 2 秒传输租期，持续流每帧续期；空闲时 OAK 内双目仍低频计算，Script 不向主机传输深度，主机不编码。只保留最新原始帧及每格式一份编码缓存，同帧多客户端复用，不积压历史视频。RGB/深度的流及深度图像请求合计最多 4 个并发，状态/设置仍可使用其余 HTTP 客户槽位。深度故障返回 503 或关闭该流，RGB 无故障时继续可用；断开、停服及设置重建立即清除深度缓存。OpenCV 单线程执行，减少与导航的 CPU 竞争。2026-10-23 目标机真实深度及 RGB/照片/20 秒录像并行通过，录像实测 26.175 FPS、深度 9.943 FPS；这不替代长期混合负载、USB/SDK 硬故障和实际无线验收，详见项目说明。

DepthAI 2.31.1 的 XLinkOut::setFpsLimit 在该设备曾导致停流，当前使用设备 Script 选完整帧后缩放/传输。SDK 打开/销毁使用自身超时，不能保证物理设备/SDK 卡死时具有假源相同停止时延。最多 16 客户，socket 读写超时 2 秒，退出中断客户并回收线程。更多参数见 `--help`，非法数值/关系明确退出。

## 运维和部署

2026-10-17 已统一部署至 `/home/nvidia/tenco-project`：源码为 `oak-camera-service/`，构建为 `build/camera/`，在线程序为 `install/camera/oak_camera_service`，systemd 为 `tenco-oak-camera.service`。照片/录像保存至源码目录的 `Capture/`；旧 `/opt/tenco/media` 的历史媒体移至 `Capture/legacy/` 并保留旧路径兼容链接，保护全部业务媒体。`Capture/`、构建产物和凭据不纳入版本管理。

完整服务文件的唯一源码为项目 `src/deployment/tenco-oak-camera.service`，项目 `systemd/` 和 `/etc/systemd/system/` 通过链接加载。仓库的 `systemd/20-native-capture.conf` 可供媒体参数升级事务使用，保持统一启动和日志入口。上线前核对 `systemctl cat tenco-oak-camera.service` 和实际 ExecStart，不从本地文件推断现场版本。在项目根目录可使用 `bash src/deployment/project.sh build-camera`、`test`、`status`；构建不会覆盖 install/ 内的在线程序，部署脚本负责已验证候选的安装切换。

用户明确要求部署后，在已授权维护窗口使用已验证产物及 SHA256：

- `scripts/deploy_verified_service.sh VERIFIED_BINARY SHA256`：拒绝录像中切换，临时保留旧程序、原子安装并重启；调用共用事务的 `--readiness-only` 模式，连续健康/新帧和实际子进程哈希验证后删除临时旧程序，失败先恢复，不创建测试媒体。
- 媒体参数升级使用 `scripts/deploy_media_settings.py VERIFIED_BINARY SHA256 --drop-in /home/nvidia/tenco-project/oak-camera-service/systemd/20-native-capture.conf --evidence /home/nvidia/tenco-project/tmp/camera-deploy-evidence.json`，以 root 执行；同时验证原生照片、默认录像真实 >=15 FPS、完整解码和配置回退。

部署脚本只接受项目内候选与资源，事务/证据固定存放 `/home/nvidia/tenco-project/tmp`，不使用系统 `/tmp`。服务继续调用 `project.sh run camera isolated`，由统一环境和 runtime logger 启动，RGB 默认参数仍由 project.sh 管理。SHA 校验定位该 systemd ControlGroup 内真正的相机二进制子进程，不把 Python 日志包装器的 MainPID 当成相机。升级 drop-in 时内容安装到项目 `systemd/tenco-oak-camera.service.d/20-native-capture.conf`，`/etc/systemd/system/` 仅保存指向该项目文件的链接；已有未知普通文件或未知链接拒绝覆盖，其他现场 override 不清除。回退恢复原链接、程序和配置，成功后清除项目内事务临时副本。历史 `/opt` 与另一工作空间不属于脚本管理范围。

部署事务持有项目内互斥锁，并发部署明确拒绝；候选先写入同目录临时文件，完成 fsync、SHA256 读回和原子替换后再启动，复制期间候选改变或落盘失败不会启动未验证的字节。锁文件保留用于后续互斥，进程退出自动释放锁，不把存在锁文件当作仍有部署进行中。

升级和回退在原子替换前恢复原文件 uid/gid，首次安装按 systemd 的显式 User/Group 归属；root 运行事务不会把服务用户的 0600 配置变为 root 私有文件。事务记录原属主，失败保留的恢复目录包含 `original-metadata.json`，不能把 `copy2` 备份当前属主当作原配置属主。

旧脚本的持续新帧检查不能替代媒体参数升级验收。只保留当前有效版本，不保留长期重复二进制；不能使用已删除的旧备份回退，必要时从受控源码重建。部署不扩展到车辆/云台运动、USB 故障注入、删除业务媒体或修改其他进程。
