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
        if(mode() == "missing") throw std::runtime_error("test device unavailable");
        if(settings_.recording.width == 80) throw std::runtime_error("test unsupported device configuration");
    }
    OakCaptureFrame read(std::chrono::milliseconds timeout) override {
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(20)));
        const auto current = mode();
        if(current == "fault" || current == "missing") throw std::runtime_error("test X_LINK_ERROR");
        if(current == "stall") return {};
        cv::Mat image(settings_.preview.height, settings_.preview.width, CV_8UC3);
        image.setTo(cv::Scalar(sequence_++ % 255, 80, 120));
        cv::Mat yuv;
        cv::cvtColor(cv::Mat(64, 96, CV_8UC3, cv::Scalar(sequence_ % 255, 80, 120)), yuv, cv::COLOR_BGR2YUV_I420);
        const auto now = std::chrono::steady_clock::now();
        OakCaptureFrame result{image, nullptr, now, nullptr, now};
        if(current != "native-stall") result.nativeYuv = std::make_shared<const std::vector<std::uint8_t>>(yuv.data, yuv.data + yuv.total());
        const int w = settings_.recording.width ? settings_.recording.width : 96;
        const int h = settings_.recording.height ? settings_.recording.height : 64;
        cv::cvtColor(cv::Mat(h, w, CV_8UC3, cv::Scalar(sequence_ % 255, 80, 120)), yuv, cv::COLOR_BGR2YUV_I420);
        if(current != "recording-stall") result.recordingYuv = std::make_shared<const std::vector<std::uint8_t>>(yuv.data, yuv.data + yuv.total());
        return result;
    }
    OakCaptureInfo info() const override { return {"test-native", 96, 64}; }
    std::string deviceId() const override { return "test-device"; }
private:
    int sequence_ = 0;
    OakMediaSettings settings_;
};
}
std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<FakeCapture>();
}
