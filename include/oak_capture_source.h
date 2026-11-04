#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <opencv2/core.hpp>

struct OakCameraConfig;

struct OakCaptureFrame {
    cv::Mat preview;
    std::shared_ptr<const std::vector<std::uint8_t>> nativeYuv;
    std::chrono::steady_clock::time_point capturedAt{};
    std::shared_ptr<const std::vector<std::uint8_t>> recordingYuv;
    std::chrono::steady_clock::time_point recordingAt{};
    cv::Mat depthMillimeters; // CV_16UC1, 0 means invalid; never synthesized from RGB.
    std::chrono::steady_clock::time_point depthAt{};
    // Timestamp of a real, low-bandwidth sensor frame, independent of clients.
    std::chrono::steady_clock::time_point healthAt{};
};

struct OakRgbDemand {
    bool preview = false, native = false, recording = false;
};

struct OakCaptureInfo {
    std::string sensorName;
    int nativeWidth = 0;
    int nativeHeight = 0;
    double minFps = 1.42, maxFps = 30;
    bool depthSupported = false;
    int depthWidth = 0, depthHeight = 0;
};

// A source belongs exclusively to the capture thread, including destruction.
class OakCaptureSource {
public:
    virtual ~OakCaptureSource() = default;
    virtual void open(const OakCameraConfig& config) = 0;
    virtual OakCaptureFrame read(std::chrono::milliseconds timeout) = 0;
    virtual std::string deviceId() const = 0;
    virtual OakCaptureInfo info() const = 0;
    // Called only on the capture thread. Disable host depth transport while idle.
    virtual void setDepthDemand(bool) {}
    virtual void setRgbDemand(OakRgbDemand) {}
};

using OakCaptureFactory = std::function<std::unique_ptr<OakCaptureSource>()>;
std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource();
