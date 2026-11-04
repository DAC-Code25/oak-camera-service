#include "oak_camera_device.h"
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
std::string mode() {
    const auto path = std::getenv("OAK_TEST_STATE_FILE");
    if(!path) return "healthy";
    std::ifstream file(path);
    std::string value;
    file >> value;
    return value;
}
class FakeCapture final : public OakCaptureSource {
public:
    void open(const OakCameraConfig& config) override {
        settings_ = config.media;
        depthEnabled_ = config.depthEnabled; depthFps_ = config.depthFps;
        if(mode() == "missing") throw std::runtime_error("test device unavailable");
        if(settings_.recording.width == 80) throw std::runtime_error("test unsupported device configuration");
    }
    OakCaptureFrame read(std::chrono::milliseconds timeout) override {
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(20)));
        const auto current = mode();
        if(current == "fault" || current == "missing") throw std::runtime_error("test X_LINK_ERROR");
        if(current == "stall") return {};
        ++sequence_;
        cv::Mat image;
        if(rgbDemand_.preview) {
            image = cv::Mat(settings_.preview.height, settings_.preview.width, CV_8UC3);
            image.setTo(cv::Scalar(sequence_ % 255, 80, 120));
        }
        cv::Mat yuv;
        const auto now = std::chrono::steady_clock::now();
        OakCaptureFrame result{image, nullptr, now, nullptr, now};
        if(rgbDemand_.native && current != "native-stall") {
            cv::cvtColor(cv::Mat(64, 96, CV_8UC3, cv::Scalar(sequence_ % 255, 80, 120)), yuv, cv::COLOR_BGR2YUV_I420);
            result.nativeYuv = std::make_shared<const std::vector<std::uint8_t>>(yuv.data, yuv.data + yuv.total());
        }
        const int w = settings_.recording.width ? settings_.recording.width : 96;
        const int h = settings_.recording.height ? settings_.recording.height : 64;
        if(rgbDemand_.recording && current != "recording-stall") {
            cv::cvtColor(cv::Mat(h, w, CV_8UC3, cv::Scalar(sequence_ % 255, 80, 120)), yuv, cv::COLOR_BGR2YUV_I420);
            result.recordingYuv = std::make_shared<const std::vector<std::uint8_t>>(yuv.data, yuv.data + yuv.total());
        }
        if(now-lastHealth_ >= std::chrono::milliseconds(500)) { result.healthAt = now; lastHealth_ = now; }
        if(depthDemand_ && depthEnabled_ && current != "depth-stall" && current != "no-depth" &&
           now-lastDepth_ >= std::chrono::milliseconds(1000/depthFps_)) {
            result.depthMillimeters = cv::Mat(24,32,CV_16UC1,cv::Scalar(1234)).clone();
            result.depthMillimeters.at<std::uint16_t>(0,0) = 0;
            result.depthMillimeters.at<std::uint16_t>(0,2) = static_cast<std::uint16_t>(sequence_);
            result.depthAt = current == "depth-old" ? now-std::chrono::seconds(5) : now;
            lastDepth_ = now;
        }
        return result;
    }
    OakCaptureInfo info() const override { return {"test-native", 96, 64, 1.42, 30, mode() != "no-depth", 32, 24}; }
    void setDepthDemand(bool enabled) override { depthDemand_ = enabled; }
    void setRgbDemand(OakRgbDemand demand) override { rgbDemand_ = demand; }
    std::string deviceId() const override { return "test-device"; }
private:
    int sequence_ = 0;
    OakMediaSettings settings_;
    bool depthEnabled_ = true, depthDemand_ = false;
    int depthFps_ = 10;
    std::chrono::steady_clock::time_point lastDepth_{};
    std::chrono::steady_clock::time_point lastHealth_{};
    OakRgbDemand rgbDemand_;
};
}
std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<FakeCapture>();
}
