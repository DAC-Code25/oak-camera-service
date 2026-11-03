#include "oak_camera_device.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace {

std::string pathOrEmpty(const std::string& path) {
    return path;
}
std::chrono::milliseconds freshness(int configured, double fps) {
    return std::chrono::milliseconds(std::max(configured, static_cast<int>(std::ceil(1500.0/fps))));
}

}  // namespace

OakCameraDevice::OakCameraDevice(OakCameraConfig config, OakCaptureFactory factory)
    : config_(std::move(config)), factory_(std::move(factory)) {
    validateOakCameraConfig(config_);
    if(!factory_) throw std::invalid_argument("capture factory is empty");
    desiredSettings_ = config_.media;
    desiredSettings_.preview = {config_.previewWidth, config_.previewHeight, config_.previewFps};
    if(!config_.settingsFile.empty() && std::filesystem::exists(config_.settingsFile)) {
        if(std::filesystem::file_size(config_.settingsFile) > 16384)
            throw std::runtime_error("settings file exceeds 16 KiB");
        std::ifstream file(config_.settingsFile);
        const std::string text{std::istreambuf_iterator<char>(file), {}};
        const auto v = oakParseJson(text);
        if(v["schemaVersion"] != 1 || !v["revision"].isString())
            throw std::runtime_error("invalid persisted settings schema/revision");
        std::size_t used = 0; const auto rev = v["revision"].asString();
        if(rev.empty() || !std::all_of(rev.begin(), rev.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw std::runtime_error("invalid persisted revision");
        settingsRevision_ = std::stoull(rev, &used);
        if(used != rev.size() || settingsRevision_ == 0) throw std::runtime_error("invalid persisted revision");
        desiredSettings_ = oakSettingsFromJson(v["settings"]);
        persistedRevision_ = settingsRevision_;
        if(v.isMember("fallbackSettings")) rollbackSettings_ = oakSettingsFromJson(v["fallbackSettings"]);
    }
    validateOakMediaSettings(desiredSettings_);
    activeSettings_ = desiredSettings_;
}

Json::Value OakCameraDevice::settingsLocked() const {
    Json::Value v; v["schemaVersion"] = 1;
    v["revision"] = std::to_string(settingsRevision_); v["activeRevision"] = std::to_string(activeRevision_);
    v["applyState"] = applyState_; v["lastError"] = settingsError_;
    v["settings"] = oakSettingsJson(desiredSettings_);
    v["activeSettings"] = hasActiveSettings_ ? oakSettingsJson(activeSettings_) : Json::Value();
    v["cameraConnected"] = hasFreshCaptureLocked(); v["recording"] = recording_ && writer_.stats().active;
    v["persisted"] = !config_.settingsFile.empty() && persistedRevision_ == settingsRevision_;
    v["persistenceState"] = config_.settingsFile.empty() ? "memory_only" :
        v["persisted"].asBool() ? "confirmed" : "unconfirmed";
    v["persistenceError"] = persistenceError_;
    v["previewTransportActive"] = running_.load() && std::chrono::steady_clock::now() < previewDemandUntil_;
    v["nativeTransportActive"] = running_.load() && nativeDemand_;
    v["recordingTransportActive"] = running_.load() && recording_;
    return v;
}
Json::Value OakCameraDevice::settings() const {
    std::lock_guard<std::mutex> lock(mutex_); return settingsLocked();
}
Json::Value OakCameraDevice::capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value v; v["schemaVersion"] = 1; v["sensorName"] = captureInfo_.sensorName;
    v["available"] = captureInfo_.nativeWidth > 0;
    v["native"]["width"] = captureInfo_.nativeWidth; v["native"]["height"] = captureInfo_.nativeHeight;
    v["native"]["minFps"] = captureInfo_.minFps; v["native"]["maxFps"] = captureInfo_.maxFps;
    v["sensorMode"] = "native"; v["resizeMode"] = "fit_with_black_bars";
    v["photoFpsMeaning"] = "native_photo_buffer_refresh_rate_not_continuous_file_saving";
    v["recordingCodec"] = "h264"; v["recordingContainer"] = "mp4";
    v["minimumOutputFps"] = 1; v["previewMaxWidth"] = 1920; v["previewMaxHeight"] = 1080;
    v["depth"] = depthStatusLocked();
    v["resolutions"] = Json::Value(Json::arrayValue);
    for(const auto& size : std::vector<std::pair<int,int>>{{640,480},{960,540},{1280,720},{1280,960},{1920,1080},{2028,1520},
                                                         {captureInfo_.nativeWidth,captureInfo_.nativeHeight}}) {
        if(size.first <= 0 || size.first > captureInfo_.nativeWidth || size.second > captureInfo_.nativeHeight) continue;
        Json::Value r; r["width"] = size.first; r["height"] = size.second; v["resolutions"].append(r);
    }
    return v;
}
Json::Value OakCameraDevice::updateSettings(const Json::Value& request) {
    if(!request.isObject() || request["schemaVersion"] != 1 || !request["expectedRevision"].isString())
        throw std::invalid_argument("schemaVersion=1 and string expectedRevision are required");
    for(const auto& k : request.getMemberNames())
        if(k != "schemaVersion" && k != "expectedRevision" && k != "settings")
            throw std::invalid_argument("unknown request field: " + k);
    const auto next = oakSettingsFromJson(request["settings"]);
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    std::uint64_t revision;
    bool sameValue = false;
    OakMediaSettings fallback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(request["expectedRevision"].asString() != std::to_string(settingsRevision_))
            throw std::logic_error("settings_revision_conflict");
        if(applyState_ == "pending") throw std::logic_error("settings_application_pending");
        if(recording_ || writer_.stats().active) throw std::logic_error("recording_in_progress");
        if(!hasFreshCaptureLocked()) throw std::logic_error("camera_not_ready");
        validateOakMediaSettings(next, &captureInfo_);
        sameValue = oakSettingsJson(next) == oakSettingsJson(desiredSettings_);
        if(sameValue && (config_.settingsFile.empty() || persistedRevision_ == settingsRevision_)) return settingsLocked();
        revision = sameValue ? settingsRevision_ : settingsRevision_ + 1;
        if(revision == 0) throw std::logic_error("settings_revision_exhausted");
        fallback = activeSettings_;
    }
    const auto persisted = oakPersistSettings(config_.settingsFile, next, revision,
        sameValue ? nullptr : &fallback, config_.settingsDirectoryOps.get());
    if(!persisted.error.empty()) std::cerr << makeTimestamp() << " 相机设置已替换，持久化未确认：" << persisted.error << std::endl;
    std::lock_guard<std::mutex> lock(mutex_);
    if(sameValue) {
        persistedRevision_ = persisted.confirmed ? revision : 0; persistenceError_ = persisted.error;
        return settingsLocked(); // Explicit durability retry, no new revision or device restart.
    }
    desiredSettings_ = next; settingsRevision_ = revision; applyState_ = "pending"; settingsError_.clear();
    persistedRevision_ = persisted.confirmed ? revision : 0; persistenceError_ = persisted.error;
    rollbackSettings_ = fallback;
    reconfigure_ = true; cameraConnected_ = false; clearFramesLocked(); frameCv_.notify_all();
    return settingsLocked();
}

OakCameraDevice::~OakCameraDevice() {
    stop();
}

void OakCameraDevice::start() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if(running_.load()) return;
    std::filesystem::create_directories(config_.snapshotDir);
    std::filesystem::create_directories(config_.recordingDir);
    running_.store(true);
    try {
        worker_ = std::thread(&OakCameraDevice::runCaptureLoop, this);
    } catch(...) {
        running_.store(false);
        throw;
    }
}

void OakCameraDevice::stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    running_.store(false);
    frameCv_.notify_all();
    if(worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    writer_.stop();
    recording_ = false;
    cameraConnected_ = false;
    clearFramesLocked();
    deviceId_.clear();
    lastMessage_ = "stopped";
    frameCv_.notify_all();
}

OakStatus OakCameraDevice::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentStatusLocked();
}

OakStatus OakCameraDevice::savePhoto() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    std::shared_ptr<const std::vector<std::uint8_t>> native;
    OakCaptureInfo info;
    OakOutputSettings photo;
    std::filesystem::path outputPath;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(!hasFreshCaptureLocked() || applyState_ == "pending") {
            auto result = currentStatusLocked(); result.success = false;
            result.message = "camera not ready or settings pending"; return result;
        }
        const auto requestedAt = std::chrono::steady_clock::now();
        const auto revision = activeRevision_;
        nativeDemand_ = true;
        const auto timeout = std::chrono::milliseconds(std::min(config_.noFrameTimeoutMs + 250,
            static_cast<int>(freshness(config_.frameFreshnessMs, activeSettings_.photo.fps).count()) + 500));
        frameCv_.wait_for(lock, timeout, [&] {
            return !running_.load() || !cameraConnected_ || activeRevision_ != revision ||
                (hasFreshNativeFrameLocked() && lastNativeFrameTime_ >= requestedAt);
        });
        nativeDemand_ = false;
        if(!hasFreshNativeFrameLocked() || activeRevision_ != revision || lastNativeFrameTime_ < requestedAt) {
            auto result = currentStatusLocked(); result.success = false;
            result.message = "timed out waiting for a new native photo frame"; return result;
        }
        native = latestNativeYuv_; info = captureInfo_; photo = activeSettings_.photo;
        outputPath = config_.snapshotDir / ("oak_photo_" + makeTimestamp() + ".jpg");
    }
    std::vector<std::uint8_t> jpeg;
    try {
        cv::Mat yuv(info.nativeHeight * 3 / 2, info.nativeWidth, CV_8UC1,
                    const_cast<std::uint8_t*>(native->data()));
        cv::Mat bgr;
        cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
        if(photo.width > 0 && (photo.width != bgr.cols || photo.height != bgr.rows)) {
            const double scale = std::min(double(photo.width)/bgr.cols, double(photo.height)/bgr.rows);
            const int w = std::max(1, int(std::lround(bgr.cols*scale))), h = std::max(1, int(std::lround(bgr.rows*scale)));
            cv::Mat resized, canvas(photo.height, photo.width, CV_8UC3, cv::Scalar());
            cv::resize(bgr, resized, {w,h}, 0, 0, cv::INTER_AREA);
            resized.copyTo(canvas(cv::Rect((photo.width-w)/2,(photo.height-h)/2,w,h))); bgr = canvas;
        }
        if(!cv::imencode(".jpg", bgr, jpeg, {cv::IMWRITE_JPEG_QUALITY, config_.captureJpegQuality})) {
            throw std::runtime_error("native JPEG encoding failed");
        }
    } catch(const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = currentStatusLocked(); result.success = false; result.message = e.what(); return result;
    }
    std::string error;
    const auto fd = ::open(outputPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if(fd < 0) error = std::strerror(errno);
    else {
        std::size_t written = 0;
        while(written < jpeg.size()) {
            const auto count = ::write(fd, jpeg.data() + written, jpeg.size() - written);
            if(count < 0 && errno == EINTR) continue;
            if(count <= 0) { error = std::strerror(errno); break; }
            written += static_cast<std::size_t>(count);
        }
        if(error.empty() && ::fsync(fd) < 0) error = std::strerror(errno);
        if(::close(fd) < 0 && error.empty()) error = std::strerror(errno);
        if(!error.empty()) std::filesystem::remove(outputPath);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = currentStatusLocked();
    result.success = error.empty();
    result.message = error.empty() ? "photo saved" : "failed to save photo: " + error;
    if(error.empty()) { lastPhotoPath_ = outputPath.string(); result.path = lastPhotoPath_; }
    lastMessage_ = result.message;
    return result;
}

OakStatus OakCameraDevice::startRecording() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    std::filesystem::path outputPath;
    OakCaptureInfo info;
    OakOutputSettings recording;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = currentStatusLocked();
        if(!hasFreshCaptureLocked() || applyState_ == "pending") {
            result.success = false; result.message = "no fresh capture frame available or settings pending"; return result;
        }
        if(recording_ && writer_.stats().active) {
            result.message = "recording already started"; result.path = lastRecordingPath_; return result;
        }
        info = captureInfo_;
        recording = activeSettings_.recording;
        outputPath = config_.recordingDir / ("oak_record_" + makeTimestamp() + ".mp4");
    }
    try { writer_.start(outputPath, recording.width ? recording.width : info.nativeWidth,
                       recording.height ? recording.height : info.nativeHeight, recording.fps, config_.recordingCrf); }
    catch(const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = currentStatusLocked(); result.success = false;
        result.message = "failed to start recording: " + std::string(e.what()); return result;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if(!hasFreshCaptureLocked() || applyState_ == "pending") {
        lock.unlock(); writer_.stop(); lock.lock();
        auto result = currentStatusLocked(); result.success = false; result.message = "camera became unavailable while starting recording";
        return result;
    }
    recordingDemandStarted_ = std::chrono::steady_clock::now();
    recording_ = true; lastRecordingPath_ = outputPath.string(); lastMessage_ = "recording started";
    // No recording transport is enabled before the writer is ready. The first
    // newly requested frame is submitted by the capture thread before success.
    frameCv_.wait_for(lock, std::chrono::milliseconds(std::min(config_.noFrameTimeoutMs + 250, 1500)), [&] {
        return !running_.load() || !cameraConnected_ || lastRecordingFrameTime_ >= recordingDemandStarted_;
    });
    if(!hasFreshCaptureLocked() || lastRecordingFrameTime_ < recordingDemandStarted_) {
        recording_ = false;
        lock.unlock(); writer_.stop(); lock.lock();
        auto result = currentStatusLocked(); result.success = false; result.message = "timed out waiting for the first recording frame";
        return result;
    }
    auto result = currentStatusLocked(); result.success = true; result.path = lastRecordingPath_;
    return result;
}

OakStatus OakCameraDevice::stopRecording() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    { std::lock_guard<std::mutex> lock(mutex_); recording_ = false; }
    const auto recorded = writer_.stop(); // Finalize outside the HTTP/status lock.
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = currentStatusLocked();
    result.success = recorded.error.empty();
    result.message = recorded.error.empty() ? "recording stopped" : "recording failed: " + recorded.error;
    result.path = lastRecordingPath_; result.recording = false; lastMessage_ = result.message;
    return result;
}

bool OakCameraDevice::waitForNextJpeg(std::uint64_t afterFrameId, std::uint64_t& outFrameId, std::vector<std::uint8_t>& outJpeg, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if(!hasFreshCaptureLocked()) return false;
    previewDemandUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto ready = frameCv_.wait_for(lock, timeout, [&] {
        return !running_.load() || !cameraConnected_ || (hasFreshFrameLocked() && frameId_ > afterFrameId);
    });

    if(!ready || !running_.load() || !hasFreshFrameLocked() || frameId_ <= afterFrameId) {
        return false;
    }

    outFrameId = frameId_;
    outJpeg = latestJpeg_;
    return true;
}

std::optional<std::vector<std::uint8_t>> OakCameraDevice::latestJpeg() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!hasFreshFrameLocked()) {
        return std::nullopt;
    }
    return latestJpeg_;
}

bool OakCameraDevice::hasFreshDepthLocked() const {
    return hasFreshCaptureLocked() && config_.depthEnabled && captureInfo_.depthSupported &&
        !latestDepth_.empty() && lastDepthFrameTime_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - lastDepthFrameTime_ <= freshness(config_.frameFreshnessMs, config_.depthFps);
}

Json::Value OakCameraDevice::depthStatusLocked() const {
    Json::Value v;
    v["enabled"] = config_.depthEnabled;
    v["supported"] = captureInfo_.depthSupported;
    v["available"] = hasFreshDepthLocked();
    v["transportActive"] = running_.load() && config_.depthEnabled && captureInfo_.depthSupported &&
        std::chrono::steady_clock::now() < depthDemandUntil_;
    v["width"] = captureInfo_.depthWidth; v["height"] = captureInfo_.depthHeight;
    v["fpsLimit"] = config_.depthFps;
    v["frameId"] = Json::UInt64(depthFrameId_);
    v["lastFrameAgeMs"] = lastDepthFrameTime_ == std::chrono::steady_clock::time_point{} ? Json::Int64(-1) :
        Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-lastDepthFrameTime_).count());
    v["unit"] = "millimeter"; v["invalidValue"] = 0; v["pixelFormat"] = "uint16";
    v["reference"] = "rectified_left_camera"; v["alignedToRgb"] = false;
    v["visualMinMm"] = config_.depthVisualMinMm; v["visualMaxMm"] = config_.depthVisualMaxMm;
    v["message"] = !config_.depthEnabled ? "深度采集已禁用" : !cameraConnected_ ? "相机未连接" :
        !captureInfo_.depthSupported ? "当前设备不支持双目深度" : hasFreshDepthLocked() ? "深度图可用" :
        v["transportActive"].asBool() ? "等待新鲜深度帧" : "按需传输，等待客户端请求";
    return v;
}

Json::Value OakCameraDevice::depthStatus() const {
    std::lock_guard<std::mutex> lock(mutex_); return depthStatusLocked();
}

bool OakCameraDevice::waitForDepthImage(bool png, std::uint64_t afterFrameId, OakDepthImage& result,
                                       std::chrono::milliseconds timeout) {
    cv::Mat depth;
    OakDepthImage current;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(!hasFreshCaptureLocked() || !config_.depthEnabled || !captureInfo_.depthSupported) return false;
        depthDemandUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        const bool ready = frameCv_.wait_for(lock, timeout, [&] {
            return !running_.load() || !cameraConnected_ || (hasFreshDepthLocked() && depthFrameId_ > afterFrameId);
        });
        if(!ready || !hasFreshDepthLocked() || depthFrameId_ <= afterFrameId) return false;
        depth = latestDepth_; // Immutable ref-counted raw frame, bounded to one current frame.
        current.frameId = depthFrameId_; current.width = depth.cols; current.height = depth.rows;
        current.capturedMonotonicMs = std::chrono::duration_cast<std::chrono::milliseconds>(lastDepthFrameTime_.time_since_epoch()).count();
    }
    std::lock_guard<std::mutex> encodingLock(depthEncodingMutex_);
    auto& cached = png ? cachedDepthPng_ : cachedDepthJpeg_;
    if(cached.frameId == current.frameId && cached.bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!hasFreshDepthLocked() || std::chrono::steady_clock::now().time_since_epoch() -
           std::chrono::milliseconds(current.capturedMonotonicMs) > freshness(config_.frameFreshnessMs, config_.depthFps)) return false;
        result = cached; return true;
    }
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    if(png) {
        // Lossless native millimeters. Low compression bounds CPU cost.
        if(!cv::imencode(".png", depth, *bytes, {cv::IMWRITE_PNG_COMPRESSION, 1})) return false;
    } else {
        cv::Mat gray, visual;
        depth.convertTo(gray, CV_8U, 255.0/(config_.depthVisualMaxMm-config_.depthVisualMinMm),
                        -255.0*config_.depthVisualMinMm/(config_.depthVisualMaxMm-config_.depthVisualMinMm));
        cv::applyColorMap(gray, visual, cv::COLORMAP_TURBO);
        visual.setTo(cv::Scalar(0,0,0), depth == 0);
        if(!cv::imencode(".jpg", visual, *bytes, {cv::IMWRITE_JPEG_QUALITY, config_.depthJpegQuality})) return false;
    }
    current.bytes = bytes;
    // Encoding never holds the capture/status lock; reject frames invalidated by a disconnect.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!hasFreshDepthLocked() || std::chrono::steady_clock::now().time_since_epoch() -
           std::chrono::milliseconds(current.capturedMonotonicMs) > freshness(config_.frameFreshnessMs, config_.depthFps)) return false;
    }
    cached = current; result = std::move(current); return true;
}

void OakCameraDevice::runCaptureLoop() {
    int retryDelayMs = config_.reconnectIntervalMs;
    while(running_.load()) {
        std::unique_ptr<OakCaptureSource> source;
        std::string failure;
        OakCameraConfig sourceConfig = config_;
        std::uint64_t revision;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sourceConfig.media = desiredSettings_; revision = settingsRevision_; reconfigure_ = false;
        }
        try {
            source = factory_();
            if(!source) throw std::runtime_error("capture factory returned no source");
            source->open(sourceConfig);
            const std::string id = source->deviceId();
            const auto info = source->info();
            { std::lock_guard<std::mutex> lock(mutex_); captureInfo_ = info; }
            validateOakMediaSettings(sourceConfig.media, &info);
            auto lastValidNative = std::chrono::steady_clock::now();
            auto lastValidRecording = lastValidNative;
            auto lastValidFrame = std::chrono::steady_clock::now();
            auto lastValidHealth = lastValidFrame;
            auto healthySince = lastValidFrame;
            bool seenFrame = false;
            bool seenPreview = false, seenNative = false, seenRecording = false, seenHealth = false;
            while(running_.load()) {
                bool depthDemand;
                OakRgbDemand rgbDemand;
                {
                    std::lock_guard<std::mutex> lock(mutex_); if(reconfigure_) break;
                    const auto now = std::chrono::steady_clock::now();
                    depthDemand = config_.depthEnabled && now < depthDemandUntil_;
                    rgbDemand = {!seenPreview || now < previewDemandUntil_, !seenNative || nativeDemand_,
                                 !seenRecording || recording_};
                }
                source->setDepthDemand(depthDemand);
                source->setRgbDemand(rgbDemand);
                auto captured = source->read(std::chrono::milliseconds(100));
                const auto& image = captured.preview;
                const auto now = std::chrono::steady_clock::now();
                if(captured.healthAt != std::chrono::steady_clock::time_point{} && captured.healthAt <= now &&
                   now-captured.healthAt <= freshness(config_.frameFreshnessMs, 2)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(captured.healthAt > lastCaptureFrameTime_) {
                        lastCaptureFrameTime_ = captured.healthAt; ++captureFrameId_;
                        lastValidHealth = now; seenHealth = true;
                    }
                }
                if(!captured.depthMillimeters.empty() && depthDemand && info.depthSupported &&
                   captured.depthMillimeters.type() == CV_16UC1 && captured.depthMillimeters.cols == info.depthWidth &&
                   captured.depthMillimeters.rows == info.depthHeight && captured.depthAt != std::chrono::steady_clock::time_point{} &&
                   captured.depthAt <= now && now-captured.depthAt <= freshness(config_.frameFreshnessMs, config_.depthFps)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latestDepth_ = std::move(captured.depthMillimeters); lastDepthFrameTime_ = captured.depthAt;
                    ++depthFrameId_; frameCv_.notify_all();
                }
                if(captured.nativeYuv) {
                    if(info.nativeWidth < 2 || info.nativeHeight < 2 ||
                       captured.nativeYuv->size() != static_cast<std::size_t>(info.nativeWidth) * info.nativeHeight * 3 / 2) {
                        throw std::runtime_error("native YUV does not match sensor resolution");
                    }
                    const auto stamp = captured.capturedAt == std::chrono::steady_clock::time_point{} ? now : captured.capturedAt;
                    if(stamp <= now && now - stamp <= freshness(config_.frameFreshnessMs, sourceConfig.media.photo.fps)) {
                        lastValidNative = now; seenNative = true;
                        std::lock_guard<std::mutex> lock(mutex_);
                        if(revision != settingsRevision_) break;
                        latestNativeYuv_ = std::move(captured.nativeYuv);
                        lastNativeFrameTime_ = stamp; captureInfo_ = info; ++nativeFrameId_;
                        frameCv_.notify_all();
                    }
                }
                if(captured.recordingYuv) {
                    if(captured.recordingAt == std::chrono::steady_clock::time_point{}) captured.recordingAt = now;
                    if(captured.recordingAt <= now && now-captured.recordingAt <= freshness(config_.frameFreshnessMs, sourceConfig.media.recording.fps)) {
                    lastValidRecording = now; seenRecording = true;
                    const auto& r = sourceConfig.media.recording;
                    const int w = r.width ? r.width : info.nativeWidth, h = r.height ? r.height : info.nativeHeight;
                    if(captured.recordingYuv->size() != static_cast<std::size_t>(w)*h*3/2)
                        throw std::runtime_error("recording YUV dimensions do not match applied settings");
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(revision != settingsRevision_) break;
                    lastRecordingFrameTime_ = captured.recordingAt;
                    if(recording_ && captured.recordingAt >= recordingDemandStarted_)
                        writer_.submit(std::move(captured.recordingYuv), captured.recordingAt);
                    frameCv_.notify_all();
                    }
                }
                if(!image.empty() && rgbDemand.preview) {
                if(image.type() != CV_8UC3 || image.cols != sourceConfig.media.preview.width ||
                   image.rows != sourceConfig.media.preview.height) {
                    throw std::runtime_error("invalid BGR preview image");
                }
                auto jpeg = encodeJpeg(image);
                if(jpeg.empty()) throw std::runtime_error("JPEG encoding produced no frame");
                lastValidFrame = now; seenPreview = true;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(!running_.load()) break;
                    if(revision != settingsRevision_) break;
                    latestJpeg_ = std::move(jpeg);
                    lastFrameTime_ = now;
                    ++frameId_;
                    frameCv_.notify_all();
                }
                }
                // Idle outputs are intentionally silent. Only a demanded stream
                // has a stall deadline; the tiny real sensor health stream is
                // checked continuously, independently of HTTP polling.
                if(!rgbDemand.preview) lastValidFrame = now;
                if(!rgbDemand.native) lastValidNative = now;
                if(!rgbDemand.recording) lastValidRecording = now;
                if(now-lastValidHealth >= std::chrono::milliseconds(std::max(config_.noFrameTimeoutMs, 1000)))
                    throw std::runtime_error("capture health stalled: no valid new sensor frame within timeout");
                if(now-lastValidFrame >= std::chrono::milliseconds(config_.noFrameTimeoutMs))
                    throw std::runtime_error("preview stalled: no valid new frame within timeout");
                if(now-lastValidNative >= std::chrono::milliseconds(config_.noFrameTimeoutMs))
                    throw std::runtime_error("native capture stalled: no valid new frame within timeout");
                if(now-lastValidRecording >= std::chrono::milliseconds(config_.noFrameTimeoutMs))
                    throw std::runtime_error("recording source stalled: no valid new frame within timeout");
                const bool connected = seenHealth && seenPreview && seenNative && seenRecording;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(!running_.load() || revision != settingsRevision_) break;
                    cameraConnected_ = connected;
                    deviceId_ = id;
                    if(cameraConnected_) {
                        if(applyState_ == "pending") {
                            const auto persisted = oakPersistSettings(config_.settingsFile, sourceConfig.media, revision,
                                nullptr, config_.settingsDirectoryOps.get());
                            persistedRevision_ = persisted.confirmed ? revision : 0; persistenceError_ = persisted.error;
                            if(!persisted.error.empty()) std::cerr << makeTimestamp() << " 相机设置运行生效，持久化未确认：" << persisted.error << std::endl;
                        }
                        activeSettings_ = sourceConfig.media; hasActiveSettings_ = true; activeRevision_ = revision;
                        rollbackSettings_.reset();
                        if(applyState_ == "pending") { applyState_ = "applied"; settingsError_.clear(); }
                    }
                    if(connected && !seenFrame) lastMessage_ = "camera connected";
                    frameCv_.notify_all();
                }
                if(connected && !seenFrame) {
                    std::clog << makeTimestamp() << " 相机已连接；双目深度=" << (info.depthSupported ? "支持" : "不支持") << std::endl;
                    healthySince = now;
                    seenFrame = true;
                }
                // Short lived connections do not keep resetting the backoff.
                if(now - healthySince >= std::chrono::milliseconds(config_.noFrameTimeoutMs)) {
                    retryDelayMs = config_.reconnectIntervalMs;
                }
            }
        } catch(const std::exception& e) {
            failure = e.what();
        } catch(...) {
            failure = "unknown capture exception";
        }
        bool intentionalChange = false;
        // Wake media waiters before taking the operation lock. A photo/start
        // request may be waiting for the failing capture thread itself.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cameraConnected_ = false; frameCv_.notify_all();
        }
        {
            std::lock_guard<std::mutex> controlLock(controlMutex_);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cameraConnected_ = false;
                deviceId_.clear();
                clearFramesLocked();
                recording_ = false;
                intentionalChange = reconfigure_;
                if(!failure.empty() && applyState_ == "pending" && rollbackSettings_ && revision == settingsRevision_) {
                    settingsError_ = failure; applyState_ = "rejected";
                    desiredSettings_ = *rollbackSettings_; rollbackSettings_.reset();
                    try {
                        const auto persisted = oakPersistSettings(config_.settingsFile, desiredSettings_, settingsRevision_,
                            nullptr, config_.settingsDirectoryOps.get());
                        persistedRevision_ = persisted.confirmed ? settingsRevision_ : 0; persistenceError_ = persisted.error;
                        if(!persisted.error.empty()) std::cerr << makeTimestamp() << " 相机设置已回退，持久化未确认：" << persisted.error << std::endl;
                    } catch(const std::exception& e) {
                        persistedRevision_ = 0; persistenceError_ = e.what();
                        settingsError_ += "; rollback persistence: " + std::string(e.what());
                    }
                    intentionalChange = true;
                }
                if(!failure.empty() && running_.load()) {
                    if(applyState_ == "pending") settingsError_ = failure;
                    lastCaptureError_ = failure;
                    lastMessage_ = "capture loop error: " + failure;
                    ++reconnectCount_;
                }
                frameCv_.notify_all();
            }
            writer_.stop();
        }
        if(running_.load()) {
            // Log before SDK cleanup so a blocking destructor cannot hide the failure.
            std::cerr << makeTimestamp() << " 相机采集异常: " << failure
                      << "；将在 " << retryDelayMs << " 毫秒后重试" << std::endl;
        }
        // Device cleanup may block in the SDK. Never hold HTTP state locks here.
        source.reset();
        if(!running_.load()) break;
        if(intentionalChange) continue;
        std::unique_lock<std::mutex> lock(mutex_);
        frameCv_.wait_for(lock, std::chrono::milliseconds(retryDelayMs), [this] { return !running_.load() || reconfigure_; });
        retryDelayMs = std::min(config_.reconnectMaxIntervalMs, retryDelayMs * 2);
    }
}

bool OakCameraDevice::hasFreshFrameLocked() const {
    return hasFreshCaptureLocked() && !latestJpeg_.empty() &&
        std::chrono::steady_clock::now() - lastFrameTime_ <= freshness(config_.frameFreshnessMs, activeSettings_.preview.fps);
}

bool OakCameraDevice::hasFreshCaptureLocked() const {
    return running_.load() && cameraConnected_ && lastCaptureFrameTime_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now()-lastCaptureFrameTime_ <= freshness(config_.frameFreshnessMs, 2);
}

bool OakCameraDevice::hasFreshNativeFrameLocked() const {
    return hasFreshCaptureLocked() && latestNativeYuv_ != nullptr &&
        std::chrono::steady_clock::now() - lastNativeFrameTime_ <= freshness(config_.frameFreshnessMs, activeSettings_.photo.fps);
}

void OakCameraDevice::clearFramesLocked() {
    latestNativeYuv_.reset();
    lastNativeFrameTime_ = {};
    lastRecordingFrameTime_ = {};
    latestJpeg_.clear();
    lastFrameTime_ = {};
    lastCaptureFrameTime_ = {}; previewDemandUntil_ = {};
    nativeDemand_ = false; recordingDemandStarted_ = {};
    latestDepth_.release(); lastDepthFrameTime_ = {}; depthDemandUntil_ = {};
}

std::string OakCameraDevice::makeTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&time, &localTime);

    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    static std::atomic<std::uint64_t> sequence{0};
    oss << '_' << std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000
        << '_' << getpid() << '_' << sequence.fetch_add(1);
    return oss.str();
}

std::vector<std::uint8_t> OakCameraDevice::encodeJpeg(const cv::Mat& frame) const {
    std::vector<std::uint8_t> encoded;
    if(!cv::imencode(".jpg", frame, encoded, {cv::IMWRITE_JPEG_QUALITY, config_.jpegQuality,
                                           cv::IMWRITE_JPEG_OPTIMIZE, config_.optimizePreviewJpeg ? 1 : 0})) {
        return {};
    }
    return encoded;
}

OakStatus OakCameraDevice::currentStatusLocked() const {
    OakStatus status;
    status.success = hasFreshCaptureLocked();
    status.message = lastMessage_;
    status.cameraConnected = status.success;
    if(cameraConnected_ && !status.success) status.message = "capture health frame stale";
    const auto media = writer_.stats();
    status.recording = recording_ && media.active;
    status.recordingFrames = media.frames;
    status.recordingDroppedFrames = media.dropped;
    status.lastRecordingError = media.error;
    status.recordingActualFps = media.actualFps;
    status.sensorName = captureInfo_.sensorName;
    status.nativeWidth = captureInfo_.nativeWidth;
    status.nativeHeight = captureInfo_.nativeHeight;
    status.captureJpegQuality = config_.captureJpegQuality;
    status.recordingCrf = config_.recordingCrf;
    status.nativeFrameId = nativeFrameId_;
    if(lastNativeFrameTime_ != std::chrono::steady_clock::time_point{}) {
        status.lastNativeFrameAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastNativeFrameTime_).count();
    }
    status.path = lastRecordingPath_;
    status.deviceId = deviceId_;
    status.previewWidth = activeSettings_.preview.width;
    status.previewHeight = activeSettings_.preview.height;
    status.previewFps = activeSettings_.preview.fps;
    status.jpegQuality = config_.jpegQuality;
    status.previewJpegOptimized = config_.optimizePreviewJpeg;
    status.snapshotDir = config_.snapshotDir.string();
    status.recordingDir = config_.recordingDir.string();
    status.lastPhotoPath = lastPhotoPath_;
    status.lastRecordingPath = lastRecordingPath_;
    status.lastCaptureError = lastCaptureError_;
    status.reconnectCount = reconnectCount_;
    status.frameId = frameId_;
    status.captureFrameId = captureFrameId_;
    if(lastCaptureFrameTime_ != std::chrono::steady_clock::time_point{}) {
        status.lastCaptureFrameAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-lastCaptureFrameTime_).count();
    }
    if(lastFrameTime_ != std::chrono::steady_clock::time_point{}) {
        status.lastFrameAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastFrameTime_).count();
    }
    return status;
}

std::string oakEscapeJson(const std::string& text) {
    std::ostringstream oss;
    for(char ch : text) {
        switch(ch) {
            case '\\':
                oss << "\\\\";
                break;
            case '"':
                oss << "\\\"";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\r':
                oss << "\\r";
                break;
            case '\t':
                oss << "\\t";
                break;
            default:
                oss << ch;
                break;
        }
    }
    return oss.str();
}

std::string oakStatusToJson(const OakStatus& status) {
    std::ostringstream oss;
    oss << "{"
        << "\"success\":" << (status.success ? "true" : "false") << ","
        << "\"message\":\"" << oakEscapeJson(status.message) << "\","
        << "\"path\":\"" << oakEscapeJson(pathOrEmpty(status.path)) << "\","
        << "\"deviceId\":\"" << oakEscapeJson(status.deviceId) << "\","
        << "\"cameraConnected\":" << (status.cameraConnected ? "true" : "false") << ","
        << "\"recording\":" << (status.recording ? "true" : "false") << ","
        << "\"previewWidth\":" << status.previewWidth << ","
        << "\"previewHeight\":" << status.previewHeight << ","
        << "\"previewFps\":" << status.previewFps << ","
        << "\"jpegQuality\":" << status.jpegQuality << ","
        << "\"previewJpegOptimized\":" << (status.previewJpegOptimized ? "true" : "false") << ","
        << "\"snapshotDir\":\"" << oakEscapeJson(status.snapshotDir) << "\","
        << "\"recordingDir\":\"" << oakEscapeJson(status.recordingDir) << "\","
        << "\"lastPhotoPath\":\"" << oakEscapeJson(status.lastPhotoPath) << "\","
        << "\"lastRecordingPath\":\"" << oakEscapeJson(status.lastRecordingPath) << "\","
        << "\"lastCaptureError\":\"" << oakEscapeJson(status.lastCaptureError) << "\","
        << "\"reconnectCount\":" << status.reconnectCount << ","
        << "\"frameId\":" << status.frameId << ","
        << "\"captureFrameId\":" << status.captureFrameId << ","
        << "\"lastCaptureFrameAgeMs\":" << status.lastCaptureFrameAgeMs << ","
        << "\"captureHealthFps\":" << status.captureHealthFps << ","
        << "\"sensorName\":\"" << oakEscapeJson(status.sensorName) << "\","
        << "\"nativeWidth\":" << status.nativeWidth << ","
        << "\"nativeHeight\":" << status.nativeHeight << ","
        << "\"captureJpegQuality\":" << status.captureJpegQuality << ","
        << "\"recordingCrf\":" << status.recordingCrf << ","
        << "\"recordingCodec\":\"h264\",\"recordingContainer\":\"mp4\","
        << "\"lastNativeFrameAgeMs\":" << status.lastNativeFrameAgeMs << ","
        << "\"nativeFrameId\":" << status.nativeFrameId << ","
        << "\"recordingFrames\":" << status.recordingFrames << ","
        << "\"recordingDroppedFrames\":" << status.recordingDroppedFrames << ","
        << "\"recordingActualFps\":" << status.recordingActualFps << ","
        << "\"lastRecordingError\":\"" << oakEscapeJson(status.lastRecordingError) << "\","
        << "\"lastFrameAgeMs\":" << status.lastFrameAgeMs
        << "}";
    return oss.str();
}

void validateOakCameraConfig(const OakCameraConfig& c) {
    if(c.port < 1 || c.port > 65535 || c.previewWidth < 16 || c.previewWidth > 4096 ||
       c.previewHeight < 16 || c.previewHeight > 4096 || !std::isfinite(c.previewFps) ||
       c.previewFps < 1 || c.previewFps > 60 || c.jpegQuality < 1 || c.jpegQuality > 100 ||
       c.captureJpegQuality < 1 || c.captureJpegQuality > 100 || c.recordingCrf < 0 || c.recordingCrf > 51 ||
       c.frameFreshnessMs < 50 || c.frameFreshnessMs > 60000 ||
       c.noFrameTimeoutMs < c.frameFreshnessMs || c.noFrameTimeoutMs > 60000 ||
       c.reconnectIntervalMs < 1 || c.reconnectMaxIntervalMs < c.reconnectIntervalMs ||
       c.reconnectMaxIntervalMs > 60000 || c.snapshotDir.empty() || c.recordingDir.empty() ||
       c.depthFps < 1 || c.depthFps > 15 || c.depthJpegQuality < 1 || c.depthJpegQuality > 90 ||
       c.depthVisualMinMm < 0 || c.depthVisualMaxMm > 65535 || c.depthVisualMaxMm <= c.depthVisualMinMm) {
        throw std::invalid_argument("invalid camera dimensions, FPS, quality, timeout, retry, port or media directory");
    }
}
