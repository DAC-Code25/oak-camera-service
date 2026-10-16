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
    v["cameraConnected"] = hasFreshFrameLocked(); v["recording"] = recording_ && writer_.stats().active;
    v["persisted"] = !config_.settingsFile.empty() && persistedRevision_ == settingsRevision_;
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
    OakMediaSettings fallback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(request["expectedRevision"].asString() != std::to_string(settingsRevision_))
            throw std::logic_error("settings_revision_conflict");
        if(applyState_ == "pending") throw std::logic_error("settings_application_pending");
        if(recording_ || writer_.stats().active) throw std::logic_error("recording_in_progress");
        if(!hasFreshNativeFrameLocked()) throw std::logic_error("camera_not_ready");
        validateOakMediaSettings(next, &captureInfo_);
        if(oakSettingsJson(next) == oakSettingsJson(desiredSettings_)) return settingsLocked();
        revision = settingsRevision_ + 1;
        if(revision == 0) throw std::logic_error("settings_revision_exhausted");
        fallback = activeSettings_;
    }
    oakPersistSettings(config_.settingsFile, next, revision, &fallback);
    std::lock_guard<std::mutex> lock(mutex_);
    desiredSettings_ = next; settingsRevision_ = revision; applyState_ = "pending"; settingsError_.clear();
    persistedRevision_ = config_.settingsFile.empty() ? 0 : revision;
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
        std::lock_guard<std::mutex> lock(mutex_);
        if(!hasFreshNativeFrameLocked()) {
            auto result = currentStatusLocked(); result.success = false;
            result.message = "no fresh native frame available"; return result;
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
        if(!hasFreshNativeFrameLocked() || applyState_ == "pending" ||
           lastRecordingFrameTime_ == std::chrono::steady_clock::time_point{} ||
           std::chrono::steady_clock::now()-lastRecordingFrameTime_ > freshness(config_.frameFreshnessMs, activeSettings_.recording.fps)) {
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
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = true; lastRecordingPath_ = outputPath.string(); lastMessage_ = "recording started";
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

bool OakCameraDevice::waitForNextJpeg(std::uint64_t afterFrameId, std::uint64_t& outFrameId, std::vector<std::uint8_t>& outJpeg, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if(!running_.load() || !hasFreshFrameLocked()) return false;
    const auto ready = frameCv_.wait_for(lock, timeout, [&] {
        return !running_.load() || !cameraConnected_ || frameId_ > afterFrameId;
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
            validateOakMediaSettings(sourceConfig.media, &info);
            auto lastValidNative = std::chrono::steady_clock::now();
            auto lastValidRecording = lastValidNative;
            auto lastValidFrame = std::chrono::steady_clock::now();
            auto healthySince = lastValidFrame;
            bool seenFrame = false;
            while(running_.load()) {
                { std::lock_guard<std::mutex> lock(mutex_); if(reconfigure_) break; }
                auto captured = source->read(std::chrono::milliseconds(100));
                const auto& image = captured.preview;
                const auto now = std::chrono::steady_clock::now();
                if(now - lastValidFrame >= std::chrono::milliseconds(config_.noFrameTimeoutMs)) {
                    throw std::runtime_error("preview stalled: no valid new frame within timeout");
                }
                if(now - lastValidNative >= std::chrono::milliseconds(config_.noFrameTimeoutMs)) {
                    throw std::runtime_error("native capture stalled: no valid new frame within timeout");
                }
                if(now - lastValidRecording >= std::chrono::milliseconds(config_.noFrameTimeoutMs))
                    throw std::runtime_error("recording source stalled: no valid new frame within timeout");
                if(captured.nativeYuv) {
                    if(info.nativeWidth < 2 || info.nativeHeight < 2 ||
                       captured.nativeYuv->size() != static_cast<std::size_t>(info.nativeWidth) * info.nativeHeight * 3 / 2) {
                        throw std::runtime_error("native YUV does not match sensor resolution");
                    }
                    const auto stamp = captured.capturedAt == std::chrono::steady_clock::time_point{} ? now : captured.capturedAt;
                    if(stamp <= now && now - stamp <= freshness(config_.frameFreshnessMs, sourceConfig.media.photo.fps)) {
                        lastValidNative = now;
                        std::lock_guard<std::mutex> lock(mutex_);
                        if(revision != settingsRevision_) break;
                        latestNativeYuv_ = std::move(captured.nativeYuv);
                        lastNativeFrameTime_ = stamp; captureInfo_ = info; ++nativeFrameId_;
                    }
                }
                if(captured.recordingYuv) {
                    if(captured.recordingAt == std::chrono::steady_clock::time_point{}) captured.recordingAt = now;
                    if(captured.recordingAt > now || now-captured.recordingAt > freshness(config_.frameFreshnessMs, sourceConfig.media.recording.fps)) continue;
                    lastValidRecording = now;
                    const auto& r = sourceConfig.media.recording;
                    const int w = r.width ? r.width : info.nativeWidth, h = r.height ? r.height : info.nativeHeight;
                    if(captured.recordingYuv->size() != static_cast<std::size_t>(w)*h*3/2)
                        throw std::runtime_error("recording YUV dimensions do not match applied settings");
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(revision != settingsRevision_) break;
                    lastRecordingFrameTime_ = captured.recordingAt;
                    if(recording_) writer_.submit(std::move(captured.recordingYuv), captured.recordingAt);
                }
                if(image.empty()) continue;
                if(image.type() != CV_8UC3 || image.cols != sourceConfig.media.preview.width ||
                   image.rows != sourceConfig.media.preview.height) {
                    throw std::runtime_error("invalid BGR preview image");
                }
                auto jpeg = encodeJpeg(image);
                if(jpeg.empty()) throw std::runtime_error("JPEG encoding produced no frame");
                lastValidFrame = now;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(!running_.load()) break;
                    if(revision != settingsRevision_) break;
                    latestJpeg_ = std::move(jpeg);
                    lastFrameTime_ = now;
                    ++frameId_;
                    cameraConnected_ = latestNativeYuv_ != nullptr && lastRecordingFrameTime_ != std::chrono::steady_clock::time_point{};
                    deviceId_ = id;
                    if(cameraConnected_) {
                        if(applyState_ == "pending") {
                            oakPersistSettings(config_.settingsFile, sourceConfig.media, revision);
                            persistedRevision_ = config_.settingsFile.empty() ? 0 : revision;
                        }
                        activeSettings_ = sourceConfig.media; hasActiveSettings_ = true; activeRevision_ = revision;
                        rollbackSettings_.reset();
                        if(applyState_ == "pending") { applyState_ = "applied"; settingsError_.clear(); }
                    }
                    if(!seenFrame) lastMessage_ = "camera connected";
                    frameCv_.notify_all();
                }
                if(!seenFrame) {
                    std::clog << makeTimestamp() << " capture connected: " << id << std::endl;
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
                    try { oakPersistSettings(config_.settingsFile, desiredSettings_, settingsRevision_); }
                    catch(const std::exception& e) { settingsError_ += "; rollback persistence: " + std::string(e.what()); }
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
            std::cerr << makeTimestamp() << " capture failed: " << failure
                      << "; retry in " << retryDelayMs << " ms" << std::endl;
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
    return running_.load() && cameraConnected_ && !latestJpeg_.empty() &&
        std::chrono::steady_clock::now() - lastFrameTime_ <= freshness(config_.frameFreshnessMs, activeSettings_.preview.fps);
}

bool OakCameraDevice::hasFreshNativeFrameLocked() const {
    return hasFreshFrameLocked() && latestNativeYuv_ != nullptr &&
        std::chrono::steady_clock::now() - lastNativeFrameTime_ <= freshness(config_.frameFreshnessMs, activeSettings_.photo.fps);
}

void OakCameraDevice::clearFramesLocked() {
    latestNativeYuv_.reset();
    lastNativeFrameTime_ = {};
    lastRecordingFrameTime_ = {};
    latestJpeg_.clear();
    lastFrameTime_ = {};
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
    status.success = hasFreshFrameLocked();
    status.message = lastMessage_;
    status.cameraConnected = status.success;
    if(cameraConnected_ && !status.success) status.message = "preview frame stale";
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
       c.reconnectMaxIntervalMs > 60000 || c.snapshotDir.empty() || c.recordingDir.empty()) {
        throw std::invalid_argument("invalid camera dimensions, FPS, quality, timeout, retry, port or media directory");
    }
}
