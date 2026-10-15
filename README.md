# 工控机 OAK 相机 HTTP 服务

更新日期：2026-10-15
会话 ID：`01a13477-9b6f-7f43-8a41-5e5b77886c15`

独立 C++17/DepthAI/OpenCV 服务，部署在 Ubuntu/Jetson 工控机，提供原生输入、可配置预览/照片/录像 HTTP API。工作空间中与 `Tenco/`、`src/` 并列，本仓库独立维护服务源码、测试、部署脚本和 systemd 配置；Tenco 上位机保留客户端，本服务不链接到 Windows Qt 应用，也不作为 ROS 包构建。厂商 SDK 从目标机 `/opt/ros/humble` 获取，不复制进仓库。

2026-10-14 从 Tenco 的 `services/oak-camera/` 提取为独立项目，保留采集恢复、最新版清理和原生采集三次服务源码改动历史；导入 main 为 `32918441be25957ff515cc500028644613ca7d60`，与原 Tenco 服务树相同。见 [迁移方案与记录](docs/develop/相机服务独立项目迁移开发方案与记录.md)。统一约束从 [AGENTS](AGENTS.md) 进入，跨项目状态见 [状态快照](docs/workspace/docs/当前状态.md)。工控机现有源码/构建仍在 `/home/nvidia/tenco-oak-probe-recovery-v1`，运行 `/opt/tenco/bin/oak_camera_service`；本次媒体参数任务将独立仓库源码同步并部署到该目录，实际验证和部署哈希见相机媒体参数开发记录。`Capture/`、SDK、构建产物和凭据不进入版本管理。

原服务来自目标机 `/home/nvidia/tenco-oak-probe`（已按清理约定删除）。历史记录描述当时的仓库归属，后续开发以本项目为准。源码首次纳管与原始哈希见 [开发方案](docs/history/OAK相机采集恢复开发方案.md) 和 [诊断记录](docs/history/相机断流排查开发记录.md)。未纳管原工程的 build、PDF、媒体、安装脚本或第三方代码。

在 Ubuntu 22.04 / Jetson ARM64（已有 DepthAI 2.31.1、OpenCV 4.8）构建：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=/opt/ros/humble
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

`oak_capture_recovery` 测试真实恢复循环的设备缺失、读帧异常、卡帧、缓存、录像不自动续录及停止/重开。`oak_http_recovery` 运行同一 HTTP/恢复代码、替换为测试采集源，验证多帧、503/EOF、故障重连、媒体接口及客户连接回收。测试不占用 USB，也不操作原在线服务。实际 USB/设备故障恢复须部署后另验收。

新接口：`GET /camera/capabilities`、`GET/PUT /camera/settings`；完整字段、并发、持久化、curl/Python调用和运维见 [接口调用说明](docs/相机媒体参数与拍摄接口调用说明.md)。Tenco入口为配置维护 → 视频与相机。

兼容接口：`GET /camera/status`、`GET /camera/stream.mjpeg`、`POST /camera/photo`、`POST /camera/record/start`、`POST /camera/record/stop`。status 的原字段保留；新增 `frameId`（重连不倒退）、`lastFrameAgeMs`（无帧 -1）、`reconnectCount`（累计采集失败）、`lastCaptureError`（恢复后仍保留最近故障）。健康状态要求收到合法新帧，使用单调时钟；未连接或帧过期的流返回 503，不发旧缓存。

默认画面保持 960×540@15、JPEG 70。采集连续 5 秒无合法新帧则重建；帧超过 1 秒即不作为正常图像提供；失败退避 1、2、4、8、10 秒，持续健康 5 秒后复位退避。失败关闭当前录像，恢复后不会自动续录。

当前默认：IMX378原生4056×3040@30输入；预览960×540@15、照片原生JPEG90/照片源2FPS、录像1280×720@30目标/H.264 yuv420p MP4。设备端选帧与缩放降低USB/CPU负载，尺寸比例不一致时完整画面补黑边。原生12MP录像可选但不保证15FPS；默认低分辨率档位通过真实文件FPS与解码验收。使用真实时间戳，不填重复帧。有界3帧编码队列、CRF23、4线程保持，队列满丢最旧帧并统计。原生照片源、录像源和预览5秒无新帧均重建，录像不自动续录；低FPS新鲜度按至少1.5个输出周期放宽。

配置在采集线程重建生效，HTTP不等待SDK打开/销毁。PUT校验expectedRevision并持久化后返回202，GET区分pending/applied/rejected和实际activeSettings；失败回退上一有效配置，改参期间退出也保留配置回退。录像中/未就绪拒改；不得在客户端启动或本地保存时无条件覆盖工控机配置。

`--capture-jpeg-quality` 控制照片质量（1～100），`--recording-crf` 控制录像质量（0～51，越小文件越大）。默认两个保存目录均为当前工作目录下Capture；目标机必须安装本目录的 `systemd/20-native-capture.conf` 到 `/etc/systemd/system/tenco-oak-camera.service.d/20-native-capture.conf`，使开机启动和手动重启使用绝对路径 `/home/nvidia/tenco-oak-probe-recovery-v1/Capture`。drop-in同时清除旧unit宽泛pkill；systemd停止自己的cgroup。既有业务媒体不迁移/删除。

构建需要pkg-config、JSONCPP、libavformat/libavcodec/libavutil开发库及含libx264的FFmpeg；测试需要ffmpeg/ffprobe。DepthAI2.31.1在该设备使用XLinkOut::setFpsLimit时原生/预览流停滞，当前采用设备Script选完整帧后再缩放/传输。历史原生12MP软件录像4～5FPS与旧部署事实保留在 [原生采集记录](docs/history/OAK原生拍照录像开发记录.md)，不作为当前默认录像档位。

status新增sensorName/nativeWidth/nativeHeight、captureJpegQuality/recordingCrf、recordingCodec/recordingContainer、nativeFrameId/lastNativeFrameAgeMs、recordingFrames/recordingDroppedFrames/recordingActualFps/lastRecordingError、mediaConfiguration；recording仅在编码线程仍活动时为true。编码线程仅持有媒体状态短锁，状态/MJPEG不等待磁盘封装。正常停止会等待有界队列编码并封装，分片减少异常退出对整段文件的影响，不承诺断电零丢失。

新增参数：`--frame-freshness-ms`、`--no-frame-timeout-ms`、`--reconnect-interval-ms`、`--reconnect-max-interval-ms`，其他参数见 `--help`；数值/关系校验失败明确退出。每次采集连接/失败写带时间的日志。最多 16 个客户连接，socket 读写超时 2 秒，退出中断客户并回收线程。

DepthAI Device 的打开/销毁使用 SDK 自身超时，无法保证物理设备/SDK 卡死时与假采集源相同的停止时延；systemd 超时兜底和真实设备开关/掉线须现场验证。HTTP 服务成功不等于 USB 供电、线材或相机硬件缺陷已解决。

部署前先查 `systemctl cat tenco-oak-camera.service`，确认 ExecStart 指向 `/opt/tenco/bin/oak_camera_service`；执行 `scripts/deploy_verified_service.sh` 时传入经过验证的二进制和期望 SHA256，在已授权维护窗口通过sudo运行。脚本拒绝录像中的切换，暂存旧程序、原子替换并重启相机服务；失败先恢复旧程序。连续三次健康且frameId增长、安装哈希一致后，删除本次临时旧程序，成功回退后也删除重复副本，只保留当前有效版本。不会安装依赖、pkill其他程序、改当前systemd/Lua/车辆控制或删除业务媒体。

2026-10-13用户要求清理长期旧版本/备份；原 `/home/nvidia/tenco-oak-probe` 与上线旧程序备份进入清理范围，最新版源码/Release仍在 `/home/nvidia/tenco-oak-probe-recovery-v1`。当前恢复方法以受控源码重新构建/部署为准，不能使用已删除备份。实际清单与验证见 [清理记录](docs/history/相机旧版本与备份清理开发记录.md)。

2026-10-15媒体参数升级使用 `scripts/deploy_media_settings.py VERIFIED_BINARY SHA256 --drop-in systemd/20-native-capture.conf --evidence /绝对路径/evidence.json`，以root在已授权相机维护窗口执行。该事务同时安装二进制与drop-in，验证原生照片、默认720p实际至少15FPS、完整解码、持续新帧与运行哈希，失败恢复程序/drop-in/设置；成功只保留最新版。旧脚本只做持续新帧验证，不替代此次媒体参数升级验收。详见 [开发方案](docs/develop/相机媒体参数接口开发方案.md) 与 [开发记录](docs/develop/相机媒体参数接口开发记录.md)。
