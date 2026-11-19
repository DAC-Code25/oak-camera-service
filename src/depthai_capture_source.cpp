#include "oak_capture_source.h"
#include "oak_camera_device.h"
#include "oak_rgb_gate_script.h"
#include "oak_h264_headers.h"

#include <depthai/depthai.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <thread>
#include <algorithm>
#include <cmath>

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
        // Bounded consumers share ISP frames. Eight native 12MP buffers alone
        // exhaust DDR when high-resolution encoding and stereo are enabled.
        camera->setIspNumFramesPool(4);
        validateOakMediaSettings(config.media, &info_);
        camera->setFps(config.media.sensorFps);
        auto rgbControl = pipeline.create<dai::node::XLinkIn>();
        rgbControl->setStreamName("rgb-demand");
        auto gate = [&](double fps, int demandIndex) -> dai::Node::Output& {
            // DepthAI 2.31.1 XLinkOut::setFpsLimit stalled the native and preview
            // streams on this IMX378. Select whole frames before resize/USB.
            auto script = pipeline.create<dai::node::Script>();
            script->inputs["in"].setQueueSize(1); script->inputs["in"].setBlocking(false);
            if(demandIndex >= 0) {
                script->inputs["control"].setQueueSize(1); script->inputs["control"].setBlocking(false);
                rgbControl->out.link(script->inputs["control"]);
            }
            script->setScript(oakRgbGateScript(fps, config.media.sensorFps, demandIndex));
            camera->isp.link(script->inputs["in"]);
            return script->outputs["out"];
        };
        auto resized = [&](const std::string& name, const OakOutputSettings& settings, dai::ImgFrame::Type type, int demandIndex) {
            auto manip = pipeline.create<dai::node::ImageManip>();
            manip->initialConfig.setResizeThumbnail(settings.width, settings.height);
            manip->initialConfig.setFrameType(type);
            manip->setMaxOutputFrameSize(settings.width*settings.height*3/2);
            manip->setNumFramesPool(2);
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
        // Keep YUV through device resize and USB; host conversion frees shared
        // OAK color-conversion resources and halves preview transport traffic.
        resized("preview", config.media.preview, dai::ImgFrame::Type::YUV420p, 0);
        auto nativeOutput = pipeline.create<dai::node::XLinkOut>();
        nativeOutput->setStreamName("native");
        nativeOutput->input.setQueueSize(1);
        nativeOutput->input.setBlocking(false);
        // Photos retain untouched native ISP pixels, independently of the
        // compressed recording path.
        gate(config.media.photo.fps, 1).link(nativeOutput->input);
        recordingWidth_ = config.media.recording.width ? config.media.recording.width : info_.nativeWidth;
        recordingHeight_ = config.media.recording.height ? config.media.recording.height : info_.nativeHeight;
        recordingEncodedWidth_ = (recordingWidth_ + 31) / 32 * 32;
        // A 4056px native image cannot be padded to 4064: ImageManip's maximum
        // output width is 4056. A clockwise quarter-turn instead gives a
        // 32-aligned 3040px JPEG width; MOV restores the upright display without
        // cropping pixels or re-encoding on the host.
        const bool rotated = recordingEncodedWidth_ > 4056 && recordingHeight_ % 32 == 0;
        recordingEncodedHeight_ = rotated ? recordingWidth_ : recordingHeight_;
        if(rotated) recordingEncodedWidth_ = recordingHeight_;
        const double pixelsPerSecond = static_cast<double>(recordingEncodedWidth_) * recordingEncodedHeight_ * config.media.recording.fps;
        if(recordingEncodedWidth_ > 4056)
            throw std::runtime_error("recording width cannot satisfy device encoder alignment within ImageManip's 4056px limit");
        // Leave headroom below the H264 4K30 limit when preview and stereo run
        // concurrently. Large outputs use the higher-throughput MJPEG profile.
        const bool h264 = !rotated && recordingWidth_ % 32 == 0 && recordingWidth_ <= 4096 && pixelsPerSecond <= 124416000;
        if(!h264 && pixelsPerSecond > 450000000)
            throw std::runtime_error("recording dimensions and FPS exceed device MJPEG encoder throughput");
        info_.recordingEncoding = rotated ? OakRecordingEncoding::MjpegRotated :
            h264 ? OakRecordingEncoding::H264 : OakRecordingEncoding::Mjpeg;
        auto recordingManip = pipeline.create<dai::node::ImageManip>();
        if(rotated) {
            const double scale = std::min(static_cast<double>(recordingWidth_) / info_.nativeWidth,
                                          static_cast<double>(recordingHeight_) / info_.nativeHeight);
            const double ox = (recordingWidth_ - info_.nativeWidth * scale) / 2;
            const double oy = (recordingHeight_ - info_.nativeHeight * scale) / 2;
            const auto x0 = static_cast<float>(-ox / scale / info_.nativeWidth);
            const auto x1 = static_cast<float>((recordingWidth_ - ox) / scale / info_.nativeWidth);
            const auto y0 = static_cast<float>(-oy / scale / info_.nativeHeight);
            const auto y1 = static_cast<float>((recordingHeight_ - oy) / scale / info_.nativeHeight);
            recordingManip->initialConfig.setWarpTransformFourPoints({{x0,y1},{x0,y0},{x1,y0},{x1,y1}}, true);
            recordingManip->initialConfig.setResize(recordingEncodedWidth_, recordingEncodedHeight_);
            recordingManip->initialConfig.setWarpBorderFillColor(0, 0, 0);
        } else if(recordingEncodedWidth_ != recordingWidth_) {
            // Right-only padding for the firmware's MJPEG width alignment.
            // The host removes only this border using JPEG coefficients.
            // Inverse mapping retains the requested complete-FOV letterbox and
            // leaves its original pixels at their original output positions.
            const double scale = std::min(static_cast<double>(recordingWidth_) / info_.nativeWidth,
                                          static_cast<double>(recordingHeight_) / info_.nativeHeight);
            const double ox = (recordingWidth_ - info_.nativeWidth * scale) / 2;
            const double oy = (recordingHeight_ - info_.nativeHeight * scale) / 2;
            const auto x0 = static_cast<float>(-ox / scale / info_.nativeWidth);
            const auto x1 = static_cast<float>((recordingEncodedWidth_ - ox) / scale / info_.nativeWidth);
            const auto y0 = static_cast<float>(-oy / scale / info_.nativeHeight);
            const auto y1 = static_cast<float>((recordingHeight_ - oy) / scale / info_.nativeHeight);
            recordingManip->initialConfig.setWarpTransformFourPoints({{x0,y0},{x1,y0},{x1,y1},{x0,y1}}, true);
            recordingManip->initialConfig.setResize(recordingEncodedWidth_, recordingHeight_);
            recordingManip->initialConfig.setWarpBorderFillColor(0, 0, 0);
        // NV12 conversion alone preserves every native pixel for aligned native
        // sizes. Other aligned sizes use the established complete-FOV resize.
        } else if(recordingWidth_ != info_.nativeWidth || recordingHeight_ != info_.nativeHeight) {
            recordingManip->initialConfig.setResizeThumbnail(recordingWidth_, recordingHeight_);
        }
        recordingManip->initialConfig.setFrameType(dai::ImgFrame::Type::NV12);
        recordingManip->setMaxOutputFrameSize(recordingEncodedWidth_ * recordingEncodedHeight_ * 3 / 2);
        recordingManip->setNumFramesPool(4);
        // Dropping before encoding is safe. Keep native ISP references bounded
        // so recorder back pressure cannot exhaust the camera's frame pool.
        recordingManip->inputImage.setQueueSize(1); recordingManip->inputImage.setBlocking(false);
        gate(config.media.recording.fps, 2).link(recordingManip->inputImage);
        auto recordingEncoder = pipeline.create<dai::node::VideoEncoder>();
        recordingEncoder->setDefaultProfilePreset(config.media.recording.fps,
            h264 ? dai::VideoEncoderProperties::Profile::H264_MAIN : dai::VideoEncoderProperties::Profile::MJPEG);
        recordingEncoder->setNumFramesPool(2);
        // The automatic bitstream slots reserve raw NV12-sized buffers. Keep a
        // generous compressed-frame allowance without starving the NV12 pool.
        recordingEncoder->setMaxOutputFrameSize(h264 ? 4 * 1024 * 1024 :
            std::max(4 * 1024 * 1024, recordingEncodedWidth_ * recordingEncodedHeight_));
        recordingEncoder->input.setQueueSize(4); recordingEncoder->input.setBlocking(true);
        if(h264) {
            recordingEncoder->setNumBFrames(0);
            recordingEncoder->setKeyframeFrequency(std::max(1, static_cast<int>(std::lround(config.media.recording.fps))));
            recordingEncoder->setBitrate(static_cast<int>(std::max(2000000.0, pixelsPerSecond * 0.16)));
        } else {
            recordingEncoder->setQuality(90);
        }
        recordingManip->out.link(recordingEncoder->input);
        auto recordingOutput = pipeline.create<dai::node::XLinkOut>();
        recordingOutput->setStreamName("recording");
        // Encoded H264 packets contain references to earlier packets: bounded
        // queues apply back pressure instead of discarding those references.
        // The gate's latest-only ISP input still keeps this branch independent
        // of preview, photos and sensor-health delivery.
        recordingOutput->input.setQueueSize(8); recordingOutput->input.setBlocking(true);
        recordingEncoder->bitstream.link(recordingOutput->input);
        device_->startPipeline(pipeline);
        queue_ = device_->getOutputQueue("preview", 2, false);
        nativeQueue_ = device_->getOutputQueue("native", 2, false);
        recordingQueue_ = device_->getOutputQueue("recording", 16, true);
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
        // Preserve every encoded entry in arrival order. Taking only the latest
        // packet would discard frames and corrupt H264 reference dependencies.
        if(auto video = recordingQueue_->tryGet<dai::ImgFrame>()) {
            if(video->getType() != dai::ImgFrame::Type::BITSTREAM || video->getData().empty() ||
               video->getData().size() > static_cast<std::size_t>(recordingEncodedWidth_) * recordingEncodedHeight_ * 3 + 65536)
                throw std::runtime_error("invalid device-encoded recording payload");
            result.recordingEncoded = std::make_shared<const std::vector<std::uint8_t>>(std::move(video->getData()));
            if(info_.recordingEncoding == OakRecordingEncoding::H264)
                result.recordingEncoded = h264Headers_.prepare(std::move(result.recordingEncoded));
            result.recordingAt = video->getTimestamp();
            result.recordingSequence = video->getSequenceNum();
        }
        if(frame) {
            const int w = frame->getWidth(), h = frame->getHeight();
            if(w < 1 || h < 1 || w % 2 || h % 2 || frame->getType() != dai::ImgFrame::Type::YUV420p ||
               frame->getData().size() != static_cast<std::size_t>(w)*h*3/2)
                throw std::runtime_error("invalid preview frame");
            cv::cvtColor(cv::Mat(h*3/2,w,CV_8UC1,frame->getData().data()), result.preview, cv::COLOR_YUV2BGR_I420);
        }
        if(!frame && !result.nativeYuv && !result.recordingEncoded && result.depthMillimeters.empty() &&
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
    int recordingEncodedWidth_ = 0;
    int recordingEncodedHeight_ = 0;
    OakCaptureInfo info_;
    OakH264Headers h264Headers_;
};
}

std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<DepthAiCaptureSource>();
}
