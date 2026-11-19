#include "oak_native_recorder.h"
#include "oak_jpeg_crop.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr AVRational captureTimeBase{1, 1000000};
using Payload = std::shared_ptr<const std::vector<std::uint8_t>>;
bool mjpegEncoding(OakRecordingEncoding encoding) {
    return encoding == OakRecordingEncoding::Mjpeg || encoding == OakRecordingEncoding::MjpegRotated;
}

void check(int code, const char* operation) {
    if(code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

AVBufferRef* retain(const Payload& bytes) {
    auto* owner = new Payload(bytes);
    auto* buffer = av_buffer_create(const_cast<std::uint8_t*>(bytes->data()), bytes->size(),
        [](void* opaque, std::uint8_t*) { delete static_cast<Payload*>(opaque); },
        owner, AV_BUFFER_FLAG_READONLY);
    if(!buffer) { delete owner; throw std::bad_alloc(); }
    return buffer;
}

struct NalUnit { std::size_t offset, size; unsigned type; };
std::vector<NalUnit> annexBUnits(const std::vector<std::uint8_t>& bytes) {
    std::vector<NalUnit> units;
    std::size_t position = 0;
    auto startCode = [&](std::size_t at) -> std::size_t {
        if(at + 3 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0) {
            if(bytes[at + 2] == 1) return 3;
            if(at + 4 <= bytes.size() && bytes[at + 2] == 0 && bytes[at + 3] == 1) return 4;
        }
        return 0;
    };
    while(position < bytes.size()) {
        const auto prefix = startCode(position);
        if(!prefix) { ++position; continue; }
        const auto first = position + prefix;
        auto end = first;
        while(end < bytes.size() && !startCode(end)) ++end;
        if(first < end) units.push_back({first, end - first, static_cast<unsigned>(bytes[first] & 31)});
        position = end;
    }
    return units;
}

struct EncodingSession {
    AVFormatContext* format = nullptr;
    AVCodecContext* encoder = nullptr;
    AVFrame* input = av_frame_alloc();
    AVPacket* output = av_packet_alloc();
    AVPacket* pending = av_packet_alloc();
    AVStream* stream = nullptr;
    OakRecordingEncoding encoding = OakRecordingEncoding::Yuv420p;
    std::vector<std::uint8_t> sps, pps;
    int width = 0, height = 0;
    std::int64_t nominalDuration = 0;
    std::uint64_t writtenFrames = 0;
    std::int64_t writtenEndUs = 0;
    bool header = false;

    ~EncodingSession() {
        av_packet_free(&pending);
        av_packet_free(&output);
        av_frame_free(&input);
        avcodec_free_context(&encoder);
        if(format) {
            if(format->pb) avio_closep(&format->pb);
            avformat_free_context(format);
        }
    }
    void writeHeader() {
        AVDictionary* options = nullptr;
        if(encoding != OakRecordingEncoding::Mjpeg) {
            av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
            av_dict_set(&options, "frag_duration", "2000000", 0);
        }
        const auto result = avformat_write_header(format, &options);
        av_dict_free(&options);
        check(result, "write recording header");
        header = true;
    }
    void open(const std::filesystem::path& path, int w, int h, float fps, int crf, OakRecordingEncoding mode) {
        if(!output || !input || !pending) throw std::bad_alloc();
        if(w <= 0 || h <= 0 || w % 2 || h % 2 || !std::isfinite(fps) || fps <= 0)
            throw std::invalid_argument("invalid recording dimensions or frame rate");
        encoding = mode; width = w; height = h;
        const auto frameRate = av_d2q(fps, 1000000);
        nominalDuration = std::max<std::int64_t>(1, av_rescale_q(1, av_inv_q(frameRate), captureTimeBase));
        if(mode == OakRecordingEncoding::Yuv420p) {
            const auto* codec = avcodec_find_encoder_by_name("libx264");
            if(!codec) throw std::runtime_error("native MP4 requires libx264 encoder");
            encoder = avcodec_alloc_context3(codec);
            if(!encoder) throw std::bad_alloc();
            encoder->width = w; encoder->height = h;
            encoder->pix_fmt = AV_PIX_FMT_YUV420P;
            encoder->time_base = captureTimeBase;
            encoder->framerate = frameRate;
            encoder->gop_size = std::max(1, static_cast<int>(fps * 2));
            encoder->max_b_frames = 0;
            encoder->thread_count = 4;
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            check(av_opt_set(encoder->priv_data, "preset", "ultrafast", 0), "set H264 preset");
            check(av_opt_set(encoder->priv_data, "tune", "zerolatency", 0), "set H264 latency");
            check(av_opt_set_int(encoder->priv_data, "crf", crf, 0), "set H264 quality");
            check(avcodec_open2(encoder, codec, nullptr), "open H264 encoder");
        }
        const auto* container = mode == OakRecordingEncoding::Mjpeg ? "matroska" :
            mode == OakRecordingEncoding::MjpegRotated ? "mov" : "mp4";
        check(avformat_alloc_output_context2(&format, nullptr, container, path.c_str()), "allocate recording container");
        if(!format) throw std::runtime_error("allocate recording container failed");
        stream = avformat_new_stream(format, nullptr);
        if(!stream) throw std::bad_alloc();
        stream->time_base = captureTimeBase;
        stream->avg_frame_rate = frameRate;
        if(encoder) check(avcodec_parameters_from_context(stream->codecpar, encoder), "set recording codec");
        else {
            stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            stream->codecpar->codec_id = mode == OakRecordingEncoding::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_MJPEG;
            stream->codecpar->width = mode == OakRecordingEncoding::MjpegRotated ? h : w;
            stream->codecpar->height = mode == OakRecordingEncoding::MjpegRotated ? w : h;
            stream->codecpar->format = mode == OakRecordingEncoding::H264 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUVJ420P;
        }
        if(mode == OakRecordingEncoding::MjpegRotated) {
            // The target MOV muxer requires a positive clockwise metadata
            // angle. 270 restores the device's clockwise input and demuxes as
            // a +90 display matrix; negative metadata angles are ignored.
            check(av_dict_set(&stream->metadata, "rotate", "270", 0), "set MOV display rotation");
        }
        check(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE), "open recording file");
        // H.264 parameter sets arrive with the first IDR, before its MP4 header.
        if(mode != OakRecordingEncoding::H264) writeHeader();
    }
    bool prepare(const Payload& bytes) {
        if(encoding == OakRecordingEncoding::Yuv420p) {
            if(bytes->size() != static_cast<std::size_t>(width) * height * 3 / 2)
                throw std::runtime_error("native YUV dimensions or payload changed during recording");
            return true;
        }
        if(bytes->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("encoded recording frame is too large");
        if(mjpegEncoding(encoding)) {
            if(bytes->size() < 4 || (*bytes)[0] != 0xff || (*bytes)[1] != 0xd8 ||
                (*bytes)[bytes->size() - 2] != 0xff || bytes->back() != 0xd9)
                throw std::runtime_error("invalid encoded MJPEG recording frame");
            return true;
        }
        const auto units = annexBUnits(*bytes);
        if(units.empty()) throw std::runtime_error("encoded H264 recording frame is not Annex B");
        bool idr = false, picture = false;
        for(const auto& nal : units) {
            if(nal.type == 7 || nal.type == 8) {
                auto& stored = nal.type == 7 ? sps : pps;
                std::vector<std::uint8_t> current(bytes->begin() + nal.offset, bytes->begin() + nal.offset + nal.size);
                if(header && current != stored) throw std::runtime_error("H264 parameters changed during recording");
                stored = std::move(current);
            }
            if(nal.type == 5) idr = true;
            if(nal.type == 1 || nal.type == 5) picture = true;
        }
        if(!header) {
            if(!idr || sps.empty() || pps.empty()) return false;
            const std::size_t extraSize = sps.size() + pps.size() + 8;
            stream->codecpar->extradata = static_cast<std::uint8_t*>(av_mallocz(extraSize + AV_INPUT_BUFFER_PADDING_SIZE));
            if(!stream->codecpar->extradata) throw std::bad_alloc();
            stream->codecpar->extradata_size = static_cast<int>(extraSize);
            auto* extra = stream->codecpar->extradata;
            for(const auto* parameter : {&sps, &pps}) {
                *extra++ = 0; *extra++ = 0; *extra++ = 0; *extra++ = 1;
                std::memcpy(extra, parameter->data(), parameter->size()); extra += parameter->size();
            }
            writeHeader();
        }
        return picture;
    }
    void writePending(std::int64_t duration) {
        const auto endUs = pending->pts + duration;
        pending->duration = duration;
        av_packet_rescale_ts(pending, captureTimeBase, stream->time_base);
        pending->stream_index = stream->index;
        const auto written = av_interleaved_write_frame(format, pending);
        av_packet_unref(pending);
        check(written, "write recording packet");
        if(format->pb->error) check(format->pb->error, "recording disk write");
        ++writtenFrames; writtenEndUs = endUs;
    }
    void queuePacket() {
        if(pending->buf) {
            const auto duration = output->pts - pending->pts;
            if(duration <= 0) throw std::runtime_error("recording packet timestamp did not advance");
            writePending(duration);
        }
        av_packet_move_ref(pending, output);
    }
    void packets() {
        for(;;) {
            const auto result = avcodec_receive_packet(encoder, output);
            if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            check(result, "receive H264 packet");
            queuePacket();
        }
    }
    void write(const Payload& bytes, std::int64_t pts) {
        if(encoding != OakRecordingEncoding::Yuv420p) {
            // Compressed packet parsers require FFmpeg's zero-filled padding.
            // Copy only the compressed bytes; raw capture planes stay shared.
            check(av_new_packet(output, static_cast<int>(bytes->size())), "allocate encoded recording packet");
            std::memcpy(output->data, bytes->data(), bytes->size());
            output->pts = output->dts = pts;
            if(mjpegEncoding(encoding)) output->flags |= AV_PKT_FLAG_KEY;
            else for(const auto& nal : annexBUnits(*bytes)) if(nal.type == 5) output->flags |= AV_PKT_FLAG_KEY;
            queuePacket();
            return;
        }
        av_frame_unref(input);
        // libx264 accepts caller strides and copies into its own aligned pictures.
        // Read-only references retain native I420 until the encoder releases it.
        input->buf[0] = retain(bytes);
        input->format = encoder->pix_fmt;
        input->width = width; input->height = height;
        input->data[0] = input->buf[0]->data;
        input->data[1] = input->data[0] + static_cast<std::size_t>(width) * height;
        input->data[2] = input->data[1] + static_cast<std::size_t>(width / 2) * (height / 2);
        input->linesize[0] = width;
        input->linesize[1] = input->linesize[2] = width / 2;
        input->pts = pts;
        const auto result = avcodec_send_frame(encoder, input);
        av_frame_unref(input);
        check(result, "encode native H264 frame");
        packets();
    }
    void finish() {
        if(!header) return;
        if(encoder) { check(avcodec_send_frame(encoder, nullptr), "flush H264 encoder"); packets(); }
        if(pending->buf) writePending(nominalDuration);
        check(av_write_trailer(format), "finalize recording container");
        avio_flush(format->pb);
        if(format->pb->error) check(format->pb->error, "final recording disk write");
        check(avio_closep(&format->pb), "close recording file");
        header = false;
    }
};
}

struct OakNativeRecorder::Impl {
    struct Item { Payload bytes; std::chrono::steady_clock::time_point timestamp; };
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    std::thread thread;
    bool stopping = false;
    bool ready = false;
    std::size_t queuedBytes = 0;
    OakRecordingEncoding encoding = OakRecordingEncoding::Yuv420p;
    OakRecordingStats stats;

    void run(std::filesystem::path path, int w, int h, float fps, int crf) {
        EncodingSession session;
        std::string error;
        try {
            session.open(path, w, h, fps, crf, encoding);
            { std::lock_guard<std::mutex> lock(mutex); stats.active = true; ready = true; cv.notify_all(); }
            std::chrono::steady_clock::time_point first{};
            std::chrono::steady_clock::time_point lastReceived{};
            bool received = false;
            std::int64_t previous = -1;
            for(;;) {
                Item item;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty(); });
                    if(!stats.error.empty()) throw std::runtime_error(stats.error);
                    if(queue.empty()) break;
                    item = std::move(queue.front()); queue.pop_front();
                    queuedBytes -= item.bytes->size();
                }
                if(received && item.timestamp <= lastReceived)
                    throw std::runtime_error("recording capture timestamp did not advance");
                received = true; lastReceived = item.timestamp;
                if(encoding == OakRecordingEncoding::Mjpeg)
                    item.bytes = oakCropJpegRightPadding(std::move(item.bytes), w, h);
                else if(encoding == OakRecordingEncoding::MjpegRotated)
                    item.bytes = oakCropJpegRightPadding(std::move(item.bytes), h, w);
                if(!session.prepare(item.bytes)) continue;
                if(previous < 0) first = item.timestamp;
                const auto pts = std::chrono::duration_cast<std::chrono::microseconds>(item.timestamp - first).count();
                if(pts <= previous) throw std::runtime_error("recording capture timestamp did not advance");
                session.write(item.bytes, pts); previous = pts;
                { std::lock_guard<std::mutex> lock(mutex); stats.frames = session.writtenFrames;
                  stats.actualFps = session.writtenEndUs > 0 ? stats.frames * 1000000.0 / session.writtenEndUs : 0; }
            }
        } catch(const std::exception& e) { error = e.what(); }
        try { session.finish(); } catch(const std::exception& e) { if(error.empty()) error = e.what(); }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stats.active = false; stats.error = error; ready = true;
            stats.frames = session.writtenFrames;
            stats.actualFps = session.writtenEndUs > 0 ? stats.frames * 1000000.0 / session.writtenEndUs : 0;
            if(stats.error.empty() && stats.frames == 0) stats.error = "recording contains no frames";
            stats.dropped += queue.size(); queue.clear(); queuedBytes = 0; cv.notify_all();
        }
    }
};

OakNativeRecorder::OakNativeRecorder() : impl_(std::make_unique<Impl>()) {}
OakNativeRecorder::~OakNativeRecorder() { stop(); }
void OakNativeRecorder::start(const std::filesystem::path& path, int w, int h, float fps, int crf, OakRecordingEncoding encoding) {
    stop();
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->stats = {}; impl_->stopping = false; impl_->ready = false; impl_->encoding = encoding;
    impl_->thread = std::thread(&Impl::run, impl_.get(), path, w, h, fps, crf);
    impl_->cv.wait(lock, [&] { return impl_->ready; });
    if(!impl_->stats.error.empty()) throw std::runtime_error(impl_->stats.error);
}
void OakNativeRecorder::submit(Payload bytes, std::chrono::steady_clock::time_point timestamp) {
    if(!bytes) throw std::invalid_argument("recording frame is null");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!impl_->stats.active || impl_->stopping) return;
    if(impl_->encoding == OakRecordingEncoding::H264) {
        constexpr std::size_t maxBytes = 64 * 1024 * 1024;
        if(impl_->queue.size() >= 64 || bytes->size() > maxBytes || impl_->queuedBytes > maxBytes - bytes->size()) {
            // Losing any reference picture would corrupt subsequent P-frames.
            impl_->stats.error = "encoded H264 recording queue overflow";
            impl_->stopping = true; impl_->cv.notify_one(); return;
        }
    } else if(impl_->queue.size() >= 3) {
        impl_->queuedBytes -= impl_->queue.front().bytes->size();
        impl_->queue.pop_front(); ++impl_->stats.dropped;
    }
    impl_->queuedBytes += bytes->size();
    impl_->queue.push_back({std::move(bytes), timestamp}); impl_->cv.notify_one();
}
OakRecordingStats OakNativeRecorder::stop() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping = true; impl_->cv.notify_all(); }
    if(impl_->thread.joinable()) impl_->thread.join();
    return stats();
}
OakRecordingStats OakNativeRecorder::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->stats;
}
