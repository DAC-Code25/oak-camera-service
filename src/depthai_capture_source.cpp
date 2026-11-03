#include "oak_capture_source.h"
#include "oak_camera_device.h"

#include <depthai/depthai.hpp>
#include <stdexcept>
#include <thread>
#include <algorithm>

namespace {
class DepthAiCaptureSource final : public OakCaptureSource {
public:
    void open(const OakCameraConfig& config) override {
        device_ = std::make_unique<dai::Device>();
        bool leftMono = false, rightMono = false;
        for(const auto& feature : device_->getConnectedCameraFeatures()) {
            const bool mono = std::find(feature.supportedTypes.begin(), feature.supportedTypes.end(), dai::CameraSensorType::MONO) != feature.supportedTypes.end();
            if(feature.socket == dai::CameraBoardSocket::CAM_B && mono) leftMono = true;
            if(feature.socket == dai::CameraBoardSocket::CAM_C && mono) rightMono = true;
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
        info_.depthSupported = leftMono && rightMono;
        if(config.depthEnabled && info_.depthSupported) {
            info_.depthWidth = 640; info_.depthHeight = 400;
            auto left = pipeline.create<dai::node::MonoCamera>();
            auto right = pipeline.create<dai::node::MonoCamera>();
            left->setBoardSocket(dai::CameraBoardSocket::CAM_B);
            right->setBoardSocket(dai::CameraBoardSocket::CAM_C);
            left->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
            right->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
            left->setFps(config.depthFps); right->setFps(config.depthFps);
            auto stereo = pipeline.create<dai::node::StereoDepth>();
            stereo->setDefaultProfilePreset(dai::node::StereoDepth::PresetMode::HIGH_DENSITY);
            stereo->setLeftRightCheck(true);
            stereo->setDepthAlign(dai::CameraBoardSocket::CAM_B);
            stereo->setOutputSize(info_.depthWidth, info_.depthHeight);
            left->out.link(stereo->left); right->out.link(stereo->right);
            auto control = pipeline.create<dai::node::XLinkIn>();
            control->setStreamName("depth-demand");
            auto gate = pipeline.create<dai::node::Script>();
            gate->inputs["depth"].setQueueSize(1); gate->inputs["depth"].setBlocking(false);
            gate->inputs["control"].setQueueSize(1); gate->inputs["control"].setBlocking(false);
            control->out.link(gate->inputs["control"]);
            stereo->depth.link(gate->inputs["depth"]);
            // Stereo runs on the OAK. While idle no depth payload crosses USB and
            // no host image encoding takes place. A bounded latest-frame gate
            // avoids back pressure from wireless clients or host scheduling.
            gate->setScript("enabled = False\nwhile True:\n    frame = node.io['depth'].get()\n    control = node.io['control'].tryGet()\n    if control is not None:\n        enabled = control.getData()[0] == 1\n    if enabled:\n        node.io['out'].send(frame)\n");
            auto output = pipeline.create<dai::node::XLinkOut>();
            output->setStreamName("depth"); output->input.setQueueSize(1); output->input.setBlocking(false);
            gate->outputs["out"].link(output->input);
        }
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
        auto rgbControl = pipeline.create<dai::node::XLinkIn>();
        rgbControl->setStreamName("rgb-demand");
        auto gate = [&](double fps, int demandIndex) -> dai::Node::Output& {
            // DepthAI 2.31.1 XLinkOut::setFpsLimit stalled the native and preview
            // streams on this IMX378. Select whole frames before resize/USB.
            auto script = pipeline.create<dai::node::Script>();
            script->inputs["in"].setQueueSize(1); script->inputs["in"].setBlocking(false);
            std::string demand = "";
            if(demandIndex >= 0) {
                script->inputs["control"].setQueueSize(1); script->inputs["control"].setBlocking(false);
                rgbControl->out.link(script->inputs["control"]);
                demand = "    control = node.io['control'].tryGet()\n    if control is not None:\n        enabled = control.getData()[" + std::to_string(demandIndex) + "] == 1\n";
            }
            script->setScript("import time\nnext_at = 0.0\nenabled = " + std::string(demandIndex < 0 ? "True" : "False") +
                "\nwhile True:\n    frame = node.io['in'].get()\n" + demand +
                "    now = time.monotonic()\n    if enabled and now >= next_at:\n        node.io['out'].send(frame)\n        next_at = max(next_at + " + std::to_string(1.0/fps) + ", now)\n");
            camera->isp.link(script->inputs["in"]);
            return script->outputs["out"];
        };
        auto resized = [&](const std::string& name, const OakOutputSettings& settings, dai::ImgFrame::Type type, int demandIndex) {
            auto manip = pipeline.create<dai::node::ImageManip>();
            manip->initialConfig.setResizeThumbnail(settings.width, settings.height);
            manip->initialConfig.setFrameType(type);
            manip->setMaxOutputFrameSize(settings.width*settings.height*3);
            manip->inputImage.setQueueSize(1); manip->inputImage.setBlocking(false);
            gate(settings.fps, demandIndex).link(manip->inputImage);
            auto output = pipeline.create<dai::node::XLinkOut>();
            output->setStreamName(name);
            output->input.setQueueSize(1); output->input.setBlocking(false);
            manip->out.link(output->input);
        };
        // Transport only metadata from a real ISP frame. Extreme native->tiny
        // ImageManip scaling exceeds the IMX378 warp cache; no image resizing
        // is needed to prove sensor progress, and no pixels should cross USB.
        auto healthOutput = pipeline.create<dai::node::XLinkOut>();
        healthOutput->setStreamName("health");
        healthOutput->setMetadataOnly(true);
        healthOutput->input.setQueueSize(1); healthOutput->input.setBlocking(false);
        gate(2, -1).link(healthOutput->input);
        resized("preview", config.media.preview, dai::ImgFrame::Type::BGR888p, 0);
        auto nativeOutput = pipeline.create<dai::node::XLinkOut>();
        nativeOutput->setStreamName("native");
        nativeOutput->input.setQueueSize(1);
        nativeOutput->input.setBlocking(false);
        // Native width 4056 is not divisible by the device encoder's required 32.
        // Transport ISP YUV420 directly over USB3 without cropping or resizing.
        gate(config.media.photo.fps, 1).link(nativeOutput->input);
        recordingWidth_ = config.media.recording.width ? config.media.recording.width : info_.nativeWidth;
        recordingHeight_ = config.media.recording.height ? config.media.recording.height : info_.nativeHeight;
        if(recordingWidth_ == info_.nativeWidth && recordingHeight_ == info_.nativeHeight) {
            auto recording = pipeline.create<dai::node::XLinkOut>();
            recording->setStreamName("recording");
            recording->input.setQueueSize(1); recording->input.setBlocking(false);
            gate(config.media.recording.fps, 2).link(recording->input);
        } else resized("recording", config.media.recording, dai::ImgFrame::Type::YUV420p, 2);
        device_->startPipeline(pipeline);
        queue_ = device_->getOutputQueue("preview", 2, false);
        nativeQueue_ = device_->getOutputQueue("native", 2, false);
        recordingQueue_ = device_->getOutputQueue("recording", 2, false);
        healthQueue_ = device_->getOutputQueue("health", 1, false);
        rgbControl_ = device_->getInputQueue("rgb-demand", 1, false);
        if(config.depthEnabled && info_.depthSupported) {
            depthQueue_ = device_->getOutputQueue("depth", 1, false);
            depthControl_ = device_->getInputQueue("depth-demand", 1, false);
        }
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
        if(auto health = latest(healthQueue_)) {
            if(static_cast<int>(health->getWidth()) != info_.nativeWidth ||
               static_cast<int>(health->getHeight()) != info_.nativeHeight ||
               health->getType() != dai::ImgFrame::Type::YUV420p || !health->getData().empty())
                throw std::runtime_error("invalid capture health metadata");
            result.healthAt = health->getTimestamp();
        }
        if(depthQueue_) {
            if(auto depth = latest(depthQueue_)) {
                if(depth->getWidth() != static_cast<unsigned>(info_.depthWidth) ||
                   depth->getHeight() != static_cast<unsigned>(info_.depthHeight) ||
                   depth->getType() != dai::ImgFrame::Type::RAW16 ||
                   depth->getData().size() != static_cast<std::size_t>(info_.depthWidth)*info_.depthHeight*2)
                    throw std::runtime_error("invalid stereo uint16 depth payload");
                result.depthMillimeters = cv::Mat(info_.depthHeight, info_.depthWidth, CV_16UC1, depth->getData().data()).clone();
                result.depthAt = depth->getTimestamp();
            }
        }
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
        if(!frame && !result.nativeYuv && !result.recordingYuv && result.depthMillimeters.empty() &&
           result.healthAt == std::chrono::steady_clock::time_point{}) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return result;
    }

    void setDepthDemand(bool enabled) override {
        if(!depthControl_ || enabled == depthDemand_) return;
        auto message = std::make_shared<dai::Buffer>();
        message->setData({static_cast<std::uint8_t>(enabled ? 1 : 0)});
        if(depthControl_->send(message, std::chrono::milliseconds(0))) depthDemand_ = enabled;
    }

    void setRgbDemand(OakRgbDemand demand) override {
        const std::vector<std::uint8_t> next{static_cast<std::uint8_t>(demand.preview),
            static_cast<std::uint8_t>(demand.native), static_cast<std::uint8_t>(demand.recording)};
        if(next == rgbDemand_) return;
        auto message = std::make_shared<dai::Buffer>(); message->setData(next);
        if(rgbControl_->send(message, std::chrono::milliseconds(0))) rgbDemand_ = next;
    }

    std::string deviceId() const override { return device_->getMxId(); }
    OakCaptureInfo info() const override { return info_; }

private:
    // Queue is destroyed before the device.
    std::unique_ptr<dai::Device> device_;
    std::shared_ptr<dai::DataOutputQueue> queue_;
    std::shared_ptr<dai::DataOutputQueue> nativeQueue_;
    std::shared_ptr<dai::DataOutputQueue> recordingQueue_;
    std::shared_ptr<dai::DataOutputQueue> healthQueue_;
    std::shared_ptr<dai::DataOutputQueue> depthQueue_;
    std::shared_ptr<dai::DataInputQueue> depthControl_;
    std::shared_ptr<dai::DataInputQueue> rgbControl_;
    std::vector<std::uint8_t> rgbDemand_;
    bool depthDemand_ = false;
    int recordingWidth_ = 0, recordingHeight_ = 0;
    OakCaptureInfo info_;
};
}

std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<DepthAiCaptureSource>();
}
