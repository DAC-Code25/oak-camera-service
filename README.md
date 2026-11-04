# 工控机 OAK 相机 HTTP 服务

独立 C++17/DepthAI/OpenCV/FFmpeg 服务，在 Ubuntu/Jetson 提供预览、照片、录像及媒体参数 HTTP API。与 `Tenco/`、`src/` 并列，不链接到 Windows Qt 应用，不作为 ROS 包构建。厂商 SDK 使用目标机 `/opt/ros/humble` 已有安装。

完整工作空间的唯一约束为 [根 AGENTS.md](../AGENTS.md)，系统架构见 [系统说明](../README.md)，版本、部署与未解决问题见 [系统记录](../docs/项目总览.md)。本页集中说明相机功能、构建、接口和运维，实质变化及历史验证见 [CHANGELOG](CHANGELOG.md)。

## 构建与测试

Ubuntu 22.04/Jetson ARM64 已有 DepthAI 2.31.1、OpenCV 4.8；另需 pkg-config、JSONCPP、libavformat/libavcodec/libavutil 开发库及带 libx264 的 FFmpeg，测试需要 ffmpeg/ffprobe。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=/opt/ros/humble
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

恢复测试运行生产恢复循环，覆盖设备缺失、读帧异常、卡帧、缓存、停止/重开与录像不自动续录。HTTP 测试替换为假采集源，覆盖多帧、503/EOF、重连、媒体参数和客户连接回收；不占用 USB、不修改在线服务。物理 USB、SDK 卡死和长期故障另需现场验收。

## 接口与输出

完整字段、错误、curl/Python 调用见本页 [HTTP 调用详情](#http-调用详情)；变更与历史验证见 [CHANGELOG](CHANGELOG.md)。

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

## HTTP 调用详情

### 1. 输入、输出和默认值

IMX378 实测原生传感器模式为 **4056×3040，1.42～30 FPS**。默认始终从原生模式采集，分别生成预览、照片源和录像源；调整输出尺寸不会把相机输入切换成裁剪的 1080p/4K 模式。

| 对象 | 默认尺寸 | 默认目标 FPS | 含义 |
|---|---|---|---|
| 传感器输入 | 4056×3040 | 30 | 原生模式输入；实际能力以 capabilities 为准 |
| MJPEG 预览 | 960×540 | 15 | 预览输出，不决定照片或录像存档尺寸 |
| JPEG 照片 | 原生 4056×3040 | 2 | FPS 为照片源缓冲刷新率；POST 一次只保存一张照片 |
| MP4 录像 | 1280×720 | 30 | H.264/yuv420p、真实帧时间戳；默认档位按实际保存 ≥15 FPS 验证 |

照片质量90，录像 libx264 superfast/CRF23。照片和录像保存在 `/home/nvidia/tenco-project/oak-camera-service/Capture`。返回的 `path` 是工控机绝对路径，接口不传输整个媒体文件，也不自动下载到上位机。

历史真机测试结果见 CHANGELOG。目标帧率是选帧上限，并不保证任意尺寸、光照或混合负载均能达到该值；原生12MP软件录像此前约4～5 FPS，选择原生尺寸不保证15 FPS。服务不复制旧帧填帧，不用修改容器帧率加速播放。

所有缩放保留完整视野和宽高比，目标比例不一致时补黑边，不拉伸、不裁剪。原生4:3保存为1280×720时会有左右黑边；可选1280×960保持近似原生比例。照片的 `width=0,height=0` 表示原生尺寸，录像也支持该选择；预览不能为0。宽高必须同时为0或同时为正偶数，正数至少32，不超过实际原生尺寸；预览另限1920×1080。接口也接受满足限制的自定义尺寸，SDK不能实际建立时会报告拒绝并回退。

### 2. 接口一览

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/camera/capabilities` | 传感器原生能力、尺寸建议、编码、FPS语义 |
| GET | `/camera/settings` | 请求配置、实际生效配置、版本、应用进度 |
| PUT | `/camera/settings` | 整体替换媒体配置，包含乐观并发版本校验 |
| GET | `/camera/status` | 采集健康、帧龄、录像状态、实际帧率/丢帧和参数快照 |
| GET | `/camera/stream.mjpeg` | 连续MJPEG预览；断线后客户端重连 |
| POST | `/camera/photo` | 按当前照片尺寸保存一张JPEG |
| POST | `/camera/record/start` | 按当前录像尺寸/FPS开始MP4 |
| POST | `/camera/record/stop` | 停止并封装MP4，检查业务success字段 |

请求、响应均为UTF-8 JSON；PUT带 `Content-Type: application/json` 和正确 `Content-Length`。不支持分块上传，正文上限16KiB、头部8KiB，socket读写空闲超时2秒，最多16个客户连接。当前服务使用既有HTTP访问方式，没有新增鉴权/HTTPS；访问范围沿现场网络管理策略配置。接口不是浏览器跨域SDK。

### 3. 读取能力与配置

```bash
BASE=http://127.0.0.1:18080
curl --noproxy '*' -fsS "$BASE/camera/capabilities" | python3 -m json.tool
curl --noproxy '*' -fsS "$BASE/camera/settings" | python3 -m json.tool
```

capabilities示例（省略部分尺寸）：

```json
{
  "schemaVersion": 1,
  "available": true,
  "sensorName": "IMX378",
  "sensorMode": "native",
  "native": {"width":4056,"height":3040,"minFps":1.42,"maxFps":30},
  "minimumOutputFps": 1,
  "previewMaxWidth":1920,"previewMaxHeight":1080,
  "resolutions":[{"width":640,"height":480},{"width":1280,"height":720},{"width":4056,"height":3040}],
  "resizeMode":"fit_with_black_bars",
  "photoFpsMeaning":"native_photo_buffer_refresh_rate_not_continuous_file_saving",
  "recordingCodec":"h264","recordingContainer":"mp4"
}
```

能力未知时 `available=false`、原生尺寸0；等相机就绪后重读。FPS为数值，可有小数，浮点返回值可能为1.419999957。各输出FPS必须在1到sensorFps之间，sensorFps在原生模式实际最小/最大值之间。缩小尺寸不会把原生传感器上限提升到1080p模式的60FPS。

settings示例：

```json
{
  "schemaVersion":1,"revision":"1","activeRevision":"1",
  "applyState":"applied","lastError":"",
  "cameraConnected":true,"recording":false,"persisted":true,
  "settings":{
    "sensorFps":30,
    "preview":{"width":960,"height":540,"fps":15},
    "photo":{"width":0,"height":0,"fps":2},
    "recording":{"width":1280,"height":720,"fps":30}
  },
  "activeSettings":{
    "sensorFps":30,
    "preview":{"width":960,"height":540,"fps":15},
    "photo":{"width":0,"height":0,"fps":2},
    "recording":{"width":1280,"height":720,"fps":30}
  }
}
```

`revision`/`activeRevision`为十进制**字符串**，避免不同语言的64位数字精度差异。`settings`为当前请求/回退后配置；`activeSettings`为已收到有效帧的配置，第一次启动尚未生效时为null。`pending`期间旧activeSettings不表示新请求已生效。`persisted=true`表示该版本的文件及父目录同步已确认，不代表已应用。rename 后目录 open/fsync 失败时新 revision 已可见，返回 `persistenceState=unconfirmed`、`persisted=false` 和 `persistenceError`，仍按该 revision 应用/回退，不能以成功 HTTP 状态推断断电持久化已确认；未设置文件时为 `memory_only`。`cameraConnected`与`recording`是读时状态，写操作仍会重新核验。

预览、照片、录像均按需传输。空闲以最高 2 Hz 保留真实 ISP 帧元数据（不传像素、不缩放），`captureFrameId`、`lastCaptureFrameAgeMs`、`captureHealthFps` 用于采集健康和远程启动检查；`frameId` 仍为预览 JPEG 计数，没有订阅时可不增长。照片等待本次请求之后的新原生帧；录像先准备 writer，再开启输出并提交首帧后返回。`previewTransportActive` / `nativeTransportActive` / `recordingTransportActive` 为设置响应里的当前需求快照，启动/重配的短暂三路验证另行进行。

### 4. 修改：先读版本，再PUT，最后回读

将读取到的完整settings修改后发送，所有四组字段都必需，不支持PATCH或省略字段。不要自行推测revision。

```json
{
  "schemaVersion":1,
  "expectedRevision":"1",
  "settings":{
    "sensorFps":30,
    "preview":{"width":960,"height":540,"fps":15},
    "photo":{"width":0,"height":0,"fps":2},
    "recording":{"width":1280,"height":720,"fps":30}
  }
}
```

```bash
curl --noproxy '*' -sS -i -X PUT "$BASE/camera/settings" \
  -H 'Content-Type: application/json' --data-binary @camera-request.json
```

- **202 Accepted**：校验、并发检查与请求配置文件替换完成；另检查 `persisted` 和 `persistenceState` 确认持久化。采集线程正在重建管线，预览会短暂中断。记住返回的revision，每300～500ms GET settings，最多等待约30秒；SDK异常时可能更久，应报告结果未知并继续读取诊断。
- **200 OK**：当前配置相同，不重建；仍检查返回的applyState/activeSettings。
- **applied**：该revision已经收到三路合法新帧，activeSettings为实际生效值。
- **rejected**：管线建立失败，settings/activeSettings恢复为上一份有效配置，lastError说明原因；不把请求值继续显示为生效。回退再次连接也可能需要等待。
- **响应丢失/超时**：GET读取revision、settings和activeSettings核实，不自动重发PUT。其他程序可能已经修改配置；冲突时重新读取并由调用者决定是否再次修改。

相机未就绪、正在改参、录像进行中不允许改参。先显式停止录像，确认完成后再改。不会因改参自动停止正在录制的业务视频，也不会自动开始或续录。

### 5. 错误与持久化

| HTTP | message示例 | 调用方处理 |
|---|---|---|
| 400 | invalid JSON body / unknown settings field / invalid output dimensions/FPS | 修正正文；重复JSON键、非有限数、错误类型、缺字段、未知字段均拒绝 |
| 409 | settings_revision_conflict | 其他程序已更新；重新读取，不盲目覆盖 |
| 409 | settings_application_pending | 上次配置仍在应用；轮询结果 |
| 409 | recording_in_progress | 显式停止录像再修改 |
| 409 | camera_not_ready | 检查设备与status，等待新鲜帧 |
| 500 | settings persistence failed / cannot create settings file | 磁盘/权限异常；当前请求未被正常受理，读回核实 |
| 404 | not found | 旧服务不支持该接口或路径/方法不对 |
| 503 | MJPEG采集不可用 | 清除旧图像，退避重连，不循环显示旧帧冒充实时 |

错误JSON为 `{"success":false,"message":"..."}`。旧拍照/录像接口还包含业务 `success/message/path`；**stop即使HTTP200也须检查success和lastRecordingError**。

线上systemd以 `--settings-file /home/nvidia/tenco-project/oak-camera-service/camera-settings.json` 启动。服务先读已有有效文件，再使用CLI初始值；启动成功会保存初始配置。PUT以临时文件写入/fsync/原子rename；候选未生效期间保留上一有效配置，进程此时退出后重启仍能在候选失败时回退。该fallback是当前事务内的配置恢复信息，不是历史源码/二进制备份；应用成功或回退后移除。格式损坏时明确退出，不静默重置默认值。不要在服务运行中手工编辑此文件。

其他部署若未传settings-file，仅运行期改参，`persisted=false`，重启从启动配置重新开始。配置永久有效不代表会自动恢复录像。服务重启/采集故障都会结束录像，重连后需显式开始。

### 6. 工控机Python调用完整示例

仅依赖Python标准库；使用本地HTTP，不依赖上位机在线。本例读取当前配置，只修改录像为1280×720/30FPS；实际录制需另调用开始/停止。

```python
import json
import time
import urllib.request
import urllib.error

BASE = "http://127.0.0.1:18080"

def call(path, method="GET", value=None):
    data = None if value is None else json.dumps(value).encode("utf-8")
    req = urllib.request.Request(BASE + path, data=data, method=method,
        headers={"Content-Type":"application/json"})
    with urllib.request.urlopen(req, timeout=5) as response:
        return json.load(response)

before = call("/camera/settings")
settings = before["settings"]
settings["recording"] = {"width":1280,"height":720,"fps":30}
# 若当前sensorFps低于30，调用者应先明确选择合适的sensorFps并核对capabilities。
accepted = call("/camera/settings", "PUT", {
    "schemaVersion":1, "expectedRevision":before["revision"], "settings":settings})
revision = accepted["revision"]
deadline = time.monotonic() + 30
while True:
    current = call("/camera/settings")
    if current["revision"] != revision:
        raise RuntimeError("配置已被其他程序修改，请重新读取")
    if current["applyState"] == "rejected":
        raise RuntimeError(current["lastError"])
    if current["applyState"] == "applied":
        assert current["activeSettings"] == settings
        break
    if time.monotonic() > deadline:
        raise TimeoutError("结果尚未确认；请查询状态，不自动重发PUT")
    time.sleep(.3)
print(current)
```

捕获HTTPError读取其JSON诊断；连接异常/超时后按第4节回读。生产程序不得把GET失败当作“默认配置已生效”。C++/Qt等语言使用相同method/path/JSON；Tenco可复用 `CameraSettingsClient`，其PUT为单次请求、超时后只查询。

### 7. 拍照、录像与实测FPS

```bash
curl --noproxy '*' -fsS -X POST "$BASE/camera/photo" -H 'Content-Type: application/json' -d '{}'
curl --noproxy '*' -fsS -X POST "$BASE/camera/record/start" -H 'Content-Type: application/json' -d '{}'
curl --noproxy '*' -fsS "$BASE/camera/status" | python3 -m json.tool
curl --noproxy '*' -fsS -X POST "$BASE/camera/record/stop" -H 'Content-Type: application/json' -d '{}'
```

`recordingActualFps`按已编码帧数和真实采集时间跨度估算，是本次录像累计平均值，不是传感器配置值/瞬时吞吐；停止后保留最近统计，开始新录像清零。`recordingFrames`为已写入帧，`recordingDroppedFrames`为有界编码队列丢弃帧；设备或USB上游未输出的帧不计入该丢帧数。因此丢帧0不等于达到目标FPS。

以返回path核验实际媒体，勿仅检查 `r_frame_rate`：

```bash
ffprobe -v error -count_frames -show_streams -show_format -of json /绝对路径/录像.mp4
ffmpeg -v error -xerror -i /绝对路径/录像.mp4 -f null -
ffprobe -v error -show_streams -of json /绝对路径/照片.jpg
```

录像用 `nb_read_frames / duration` 核验平均帧率，必要时逐帧检查时间戳。手机/浏览器播放或扩展名不是编码验证依据。单张JPEG没有视频播放FPS；照片缓冲刷新2FPS不表示每秒自动写2张文件。POST响应丢失的拍照请求不要盲目重拍，应核对实际文件；任务检查点沿用Tenco既有intent/结果确认逻辑。

健康帧龄采用单调时钟；默认1秒，低帧率时按至少1.5个输出周期放宽，避免1FPS配置必然间歇失效。预览、原生照片源、录像源任一路连续5秒无有效帧均会重建；故障清空缓存、停止封装录像，不自动续录。SDK打开/销毁卡死和真实USB插拔恢复、长期混合负载仍需现场单独验收。

### 8. Tenco位置与服务运维

Tenco 在“工控机 · 服务与参数”的相机采集区读取、应用服务端尺寸/FPS并回读确认；“本机 · 相机与云台”维护服务地址、预览和本机后端。其他程序改参后先重新读取；录像期间禁用应用，本机连接设置不代替工控机保存参数。

```bash
sudo systemctl status tenco-oak-camera.service
sudo systemctl restart tenco-oak-camera.service
sudo systemctl start tenco-oak-camera.service
sudo systemctl stop tenco-oak-camera.service
journalctl -u tenco-oak-camera.service -n 100 --no-pager
```

默认开机启用状态由现有systemd维护；本次不改变启用策略。手动stop会关闭服务，摄像头插拔不能让被显式停止的服务自动启动。相机正常断流/重连可查询status；不要启动第二个程序同时占用同一相机。经验证的本次升级使用 `scripts/deploy_media_settings.py`，同时事务安装二进制/drop-in，验证真实20秒默认媒体≥15FPS后删除临时旧版本；失败恢复原二进制、drop-in、配置。媒体文件始终保留。

### 实时深度接口

深度与 RGB 共用相机服务端口。`GET /camera/depth/status` 不启动图像传输，返回 `enabled`、`supported`、`available`、`transportActive`、`width/height`、`fpsLimit`、`frameId`、`lastFrameAgeMs`、`unit`、`invalidValue`、`reference` 和可视化范围。相同状态也在 `/camera/status` 与 `/camera/capabilities` 的 `depth` 字段提供。`supported` 表示实际检测到 CAM_B/C 双目传感器，`available` 只表示当前有新鲜有效数据。

| 路径（均 GET） | 类型和用途 |
|---|---|
| `/camera/depth/image.png` | `image/png`，单通道无损 uint16 毫米值，0 无效 |
| `/camera/depth/image.jpg` | `image/jpeg`，伪彩色显示，黑色为无效 |
| `/camera/depth/stream.mjpeg` | `multipart/x-mixed-replace`，连续新深度帧的伪彩色 JPEG |

默认分辨率 640×400、10 FPS，深度坐标为 `rectified_left_camera`，未对齐到 RGB，不能按 RGB 像素下标直接索引深度。PNG 中的 16 位数值就是毫米；JPEG/MJPEG 的颜色只用于显示。默认 300～10000 mm 映射至颜色，范围外仅截断显示，不改变 PNG 数据。每张图像和 MJPEG 每个 part 提供 `X-Frame-Id`、`X-Captured-Monotonic-Ms`、`X-Depth-Unit: millimeter`、`X-Depth-Invalid-Value: 0` 与 `X-Depth-Reference`；时间是工控机单调时钟，只用于同机帧顺序/年龄，不是 UTC。

```bash
curl --fail http://工控机地址:18080/camera/depth/status
curl --fail http://工控机地址:18080/camera/depth/image.png -o depth.png
curl --fail http://工控机地址:18080/camera/depth/image.jpg -o depth.jpg
```

```python
import cv2
import numpy as np
import urllib.request
with urllib.request.urlopen("http://工控机地址:18080/camera/depth/image.png", timeout=3) as response:
    depth = cv2.imdecode(np.frombuffer(response.read(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
assert depth.dtype == np.uint16 and depth.ndim == 2
distance_mm = int(depth[depth.shape[0] // 2, depth.shape[1] // 2])
print("无效" if distance_mm == 0 else f"{distance_mm} mm")
```

图像请求触发 2 秒按需传输租期，流逐帧续期；空闲无深度 USB 传输和主机编码。每格式仅缓存最新编码，每客户端跳过旧帧，无法及时写 socket 的客户端最多 2 秒后断开。RGB/深度流及深度图像最多同时使用 4 个媒体槽位，超限 503。硬件不支持、禁用、断开、数据过期或首次等待超时时图像返回带状态 JSON 的 503，已建立的流结束，客户端按退避重新连接；不能把旧图循环播放当作恢复。深度单独无帧时不重启正常 RGB 采集。

启动选项：`--disable-depth`，`--depth-fps 1..15`（默认 10），`--depth-jpeg-quality 1..90`（默认 65），`--depth-visual-min-mm`（默认 300），`--depth-visual-max-mm`（默认 10000）。这些独立启动选项不改变现有 RGB 设置接口和持久化格式，修改后需重启相机服务。
