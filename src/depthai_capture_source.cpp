#include "oak_capture_source.h"
#include "oak_camera_device.h"

#include <depthai/depthai.hpp>
#include <stdexcept>
#include <thread>

namespace {
class DepthAiCaptureSource final : public OakCaptureSource {
public:
    void open(const OakCameraConfig& config) override {
        device_ = std::make_unique<dai::Device>();
        for(const auto& feature : device_->getConnectedCameraFeatures()) {
            if(feature.socket == dai::CameraBoardSocket::CAM_A) {
                info_ = {feature.sensorName, feature.width, feature.height};
                for(const auto& mode : feature.configs) {
                    if(mode.width == info_.nativeWidth && mode.height == info_.nativeHeight) {
                        info_.minFps = mode.minFps; info_.maxFps = mode.maxFps;
                    }
                }
            }
        }
        dai::Pipeline pipeline;
        auto camera = pipeline.create<dai::node::ColorCamera>();
        camera->setBoardSocket(dai::CameraBoardSocket::CAM_A);
        // Refuse an unrecognized native mode instead of silently using a smaller image.
        if(info_.nativeWidth == 4056 && info_.nativeHeight == 3040) {
            camera->setResolution(dai::ColorCameraProperties::SensorResolution::THE_12_MP);
        } else if(info_.nativeWidth == 4208 && info_.nativeHeight == 3120) {
            camera->setResolution(dai::ColorCameraProperties::SensorResolution::THE_13_MP);
        } else {
            throw std::runtime_error("unsupported native color sensor mode: " + info_.sensorName);
        }
        camera->setIspScale(1, 1);
        // Three consumers may retain different native frames.
        camera->setIspNumFramesPool(8);
        validateOakMediaSettings(config.media, &info_);
        camera->setFps(config.media.sensorFps);
        auto gate = [&](double fps) -> dai::Node::Output& {
            if(fps >= config.media.sensorFps) return camera->isp;
            // DepthAI 2.31.1 XLinkOut::setFpsLimit stalled the native and preview
            // streams on this IMX378. Select whole frames before resize/USB.
            auto script = pipeline.create<dai::node::Script>();
            script->inputs["in"].setQueueSize(1); script->inputs["in"].setBlocking(false);
            script->setScript("import time\nnext_at = 0.0\nwhile True:\n    frame = node.io['in'].get()\n    now = time.monotonic()\n    if now >= next_at:\n        node.io['out'].send(frame)\n        next_at = max(next_at + " + std::to_string(1.0/fps) + ", now)\n");
            camera->isp.link(script->inputs["in"]);
            return script->outputs["out"];
        };
        auto resized = [&](const std::string& name, const OakOutputSettings& settings, dai::ImgFrame::Type type) {
            auto manip = pipeline.create<dai::node::ImageManip>();
            manip->initialConfig.setResizeThumbnail(settings.width, settings.height);
            manip->initialConfig.setFrameType(type);
            manip->setMaxOutputFrameSize(settings.width*settings.height*3);
            manip->inputImage.setQueueSize(1); manip->inputImage.setBlocking(false);
            gate(settings.fps).link(manip->inputImage);
            auto output = pipeline.create<dai::node::XLinkOut>();
            output->setStreamName(name);
            output->input.setQueueSize(1); output->input.setBlocking(false);
            manip->out.link(output->input);
        };
        resized("preview", config.media.preview, dai::ImgFrame::Type::BGR888p);
        auto nativeOutput = pipeline.create<dai::node::XLinkOut>();
        nativeOutput->setStreamName("native");
        nativeOutput->input.setQueueSize(1);
        nativeOutput->input.setBlocking(false);
        // Native width 4056 is not divisible by the device encoder's required 32.
        // Transport ISP YUV420 directly over USB3 without cropping or resizing.
        gate(config.media.photo.fps).link(nativeOutput->input);
        recordingWidth_ = config.media.recording.width ? config.media.recording.width : info_.nativeWidth;
        recordingHeight_ = config.media.recording.height ? config.media.recording.height : info_.nativeHeight;
        if(recordingWidth_ == info_.nativeWidth && recordingHeight_ == info_.nativeHeight) {
            auto recording = pipeline.create<dai::node::XLinkOut>();
            recording->setStreamName("recording");
            recording->input.setQueueSize(1); recording->input.setBlocking(false);
            gate(config.media.recording.fps).link(recording->input);
        } else resized("recording", config.media.recording, dai::ImgFrame::Type::YUV420p);
        device_->startPipeline(pipeline);
        queue_ = device_->getOutputQueue("preview", 2, false);
        nativeQueue_ = device_->getOutputQueue("native", 2, false);
        recordingQueue_ = device_->getOutputQueue("recording", 2, false);
    }

    OakCaptureFrame read(std::chrono::milliseconds timeout) override {
        OakCaptureFrame result;
        (void)timeout;
        auto latest = [](const auto& queue) {
            std::shared_ptr<dai::ImgFrame> frame;
            while(auto f = queue->template tryGet<dai::ImgFrame>()) frame = std::move(f);
            return frame;
        };
        const auto frame = latest(queue_);
        if(auto native = latest(nativeQueue_)) {
            if(static_cast<int>(native->getWidth()) != info_.nativeWidth ||
               static_cast<int>(native->getHeight()) != info_.nativeHeight ||
               native->getType() != dai::ImgFrame::Type::YUV420p ||
               native->getData().size() != static_cast<std::size_t>(info_.nativeWidth) * info_.nativeHeight * 3 / 2) {
                throw std::runtime_error("invalid native ISP YUV420 dimensions or payload");
            }
            result.nativeYuv = std::make_shared<const std::vector<std::uint8_t>>(std::move(native->getData()));
            result.capturedAt = native->getTimestamp();
        }
        if(auto video = latest(recordingQueue_)) {
            if(video->getWidth() != static_cast<unsigned>(recordingWidth_) ||
               video->getHeight() != static_cast<unsigned>(recordingHeight_) ||
               video->getType() != dai::ImgFrame::Type::YUV420p ||
               video->getData().size() != static_cast<std::size_t>(recordingWidth_)*recordingHeight_*3/2)
                throw std::runtime_error("invalid recording YUV frame dimensions or payload");
            result.recordingYuv = std::make_shared<const std::vector<std::uint8_t>>(std::move(video->getData()));
            result.recordingAt = video->getTimestamp();
        }
        if(frame) {
            const int w = frame->getWidth(), h = frame->getHeight();
            if(w < 1 || h < 1 || frame->getData().size() != static_cast<std::size_t>(w)*h*3)
                throw std::runtime_error("invalid preview frame");
            std::vector<cv::Mat> planes;
            for(int n = 0; n < 3; ++n) planes.emplace_back(h,w,CV_8UC1,frame->getData().data()+n*w*h);
            cv::merge(planes, result.preview);
        }
        if(!frame && !result.nativeYuv && !result.recordingYuv) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return result;
    }

    std::string deviceId() const override { return device_->getMxId(); }
    OakCaptureInfo info() const override { return info_; }

private:
    // Queue is destroyed before the device.
    std::unique_ptr<dai::Device> device_;
    std::shared_ptr<dai::DataOutputQueue> queue_;
    std::shared_ptr<dai::DataOutputQueue> nativeQueue_;
    std::shared_ptr<dai::DataOutputQueue> recordingQueue_;
    int recordingWidth_ = 0, recordingHeight_ = 0;
    OakCaptureInfo info_;
};
}

std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<DepthAiCaptureSource>();
}
