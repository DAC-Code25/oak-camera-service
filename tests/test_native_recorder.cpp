#include "oak_native_recorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <opencv2/opencv.hpp>

namespace {
constexpr int width = 96, height = 64;
constexpr AVRational microseconds{1, 1000000};
using Payload = std::shared_ptr<const std::vector<std::uint8_t>>;

void require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
void checked(int result, const char* message) { require(result >= 0, message); }

Payload nativeFrame(int value) {
    auto frame = std::make_shared<std::vector<std::uint8_t>>(width * height * 3 / 2, 128);
    std::fill_n(frame->begin(), width * height, value);
    return frame;
}
void waitForWritten(OakNativeRecorder& recorder, std::uint64_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while(recorder.stats().frames < count && std::chrono::steady_clock::now() < deadline) {
        require(recorder.stats().error.empty(), "recording worker failed while waiting for packet");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(recorder.stats().frames >= count, "recording worker did not write expected packets");
}

struct DecodeSession {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    ~DecodeSession() {
        av_frame_free(&frame); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&format);
    }
};

void verifyDecoded(const std::filesystem::path& path, const std::vector<std::int64_t>& expectedPts,
                   const std::vector<int>& expectedLuma, std::int64_t durationUs, int timestampTolerance = 1,
                   int codedWidth = width, int codedHeight = height) {
    DecodeSession session;
    require(session.packet && session.frame, "decoder allocation failed");
    checked(avformat_open_input(&session.format, path.c_str(), nullptr, nullptr), "saved container cannot be opened");
    checked(avformat_find_stream_info(session.format, nullptr), "saved stream info missing");
    const auto index = av_find_best_stream(session.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    checked(index, "saved container lacks video stream");
    auto* stream = session.format->streams[index];
    const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    require(codec, "saved codec cannot be decoded");
    session.codec = avcodec_alloc_context3(codec);
    require(session.codec, "decoder context allocation failed");
    checked(avcodec_parameters_to_context(session.codec, stream->codecpar), "decoder parameters invalid");
    session.codec->thread_count = 1;
    checked(avcodec_open2(session.codec, codec, nullptr), "decoder cannot open saved codec");
    std::size_t decoded = 0;
    auto drain = [&] {
        for(;;) {
            const auto result = avcodec_receive_frame(session.codec, session.frame);
            if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            checked(result, "saved frame failed full decode");
            require(decoded < expectedPts.size(), "saved recording duplicated frames");
            require(session.frame->width == codedWidth && session.frame->height == codedHeight, "decoded dimensions changed");
            const auto pts = av_rescale_q(session.frame->best_effort_timestamp, stream->time_base, microseconds);
            require(std::abs(pts - expectedPts[decoded]) <= timestampTolerance, "capture timing was compressed or rounded incorrectly");
            const auto luma = session.frame->data[0][codedHeight / 2 * session.frame->linesize[0] + codedWidth / 2];
            require(std::abs(static_cast<int>(luma) - expectedLuma[decoded]) <= 3, "recording repeated or lost native image content");
            ++decoded; av_frame_unref(session.frame);
        }
    };
    for(;;) {
        const auto result = av_read_frame(session.format, session.packet);
        if(result == AVERROR_EOF) break;
        checked(result, "saved packet cannot be read");
        if(session.packet->stream_index == index) {
            checked(avcodec_send_packet(session.codec, session.packet), "saved packet cannot be decoded"); drain();
        }
        av_packet_unref(session.packet);
    }
    checked(avcodec_send_packet(session.codec, nullptr), "decoder flush failed"); drain();
    require(decoded == expectedPts.size(), "saved recording lost frames");
    require(session.format->duration > 0 && std::abs(session.format->duration - durationUs) <= timestampTolerance + 1,
            "container duration does not preserve actual capture timing");
    const auto actualFps = decoded * 1000000.0 / session.format->duration;
    const auto expectedFps = expectedPts.size() * 1000000.0 / durationUs;
    const auto fpsTolerance = expectedFps * (timestampTolerance + 1) / durationUs + 0.0001;
    require(std::abs(actualFps - expectedFps) < fpsTolerance, "decoded actual FPS differs from capture timing");
}

struct FixtureEncoder {
    AVCodecContext* context = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    ~FixtureEncoder() { av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context); }
};
std::vector<Payload> encodedFixtures(bool h264) {
    FixtureEncoder fixture;
    const auto* codec = h264 ? avcodec_find_encoder_by_name("libx264") : avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    require(codec && fixture.frame && fixture.packet, "fixture codec missing");
    fixture.context = avcodec_alloc_context3(codec);
    require(fixture.context, "fixture context allocation failed");
    auto* context = fixture.context;
    context->width = width; context->height = height;
    context->pix_fmt = h264 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUVJ420P;
    context->color_range = h264 ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
    context->time_base = {1, 30}; context->framerate = {30, 1};
    context->gop_size = 4; context->max_b_frames = 0; context->thread_count = 1;
    if(h264) {
        checked(av_opt_set(context->priv_data, "preset", "ultrafast", 0), "fixture preset failed");
        checked(av_opt_set(context->priv_data, "tune", "zerolatency", 0), "fixture latency failed");
        checked(av_opt_set(context->priv_data, "x264-params", "repeat-headers=1:scenecut=0:keyint=4:min-keyint=4", 0),
                "fixture repeat headers failed");
    }
    checked(avcodec_open2(context, codec, nullptr), "fixture encoder failed");
    fixture.frame->format = context->pix_fmt; fixture.frame->width = width; fixture.frame->height = height;
    fixture.frame->color_range = context->color_range;
    checked(av_frame_get_buffer(fixture.frame, 32), "fixture frame allocation failed");
    std::vector<Payload> packets;
    auto drain = [&] {
        for(;;) {
            const auto result = avcodec_receive_packet(context, fixture.packet);
            if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            checked(result, "fixture packet failed");
            packets.push_back(std::make_shared<const std::vector<std::uint8_t>>(
                fixture.packet->data, fixture.packet->data + fixture.packet->size));
            av_packet_unref(fixture.packet);
        }
    };
    for(int i = 0; i < 9; ++i) {
        checked(av_frame_make_writable(fixture.frame), "fixture frame not writable");
        for(int plane = 0; plane < 3; ++plane)
            for(int row = 0; row < (plane == 0 ? height : height / 2); ++row)
                std::memset(fixture.frame->data[plane] + row * fixture.frame->linesize[plane],
                    plane == 0 ? 30 + i * 16 : 128, plane == 0 ? width : width / 2);
        fixture.frame->pts = i;
        checked(avcodec_send_frame(context, fixture.frame), "fixture encode failed"); drain();
    }
    checked(avcodec_send_frame(context, nullptr), "fixture flush failed"); drain();
    require(packets.size() == 9, "fixture encoder did not create one packet per frame");
    return packets;
}

void testNative(OakNativeRecorder& recorder, const std::filesystem::path& dir) {
    bool rejected = false;
    try { recorder.start(dir / "missing" / "bad.mp4", width, height, 15, 23); }
    catch(const std::exception&) { rejected = true; }
    require(rejected, "file open failure accepted"); recorder.stop();

    // A sparse source must remain sparse in the saved file, regardless of target FPS.
    recorder.start(dir / "native.mp4", width, height, 15, 23);
    const auto start = std::chrono::steady_clock::time_point{};
    std::vector<std::int64_t> pts;
    std::vector<int> luma;
    std::vector<std::weak_ptr<const std::vector<std::uint8_t>>> released;
    for(int i = 0; i < 8; ++i) {
        pts.push_back(i * 200000 + (i ? 123 : 0)); luma.push_back(30 + i * 16);
        auto native = nativeFrame(luma.back()); released.push_back(native);
        recorder.submit(native, start + std::chrono::microseconds(pts.back())); native.reset();
        if(i < 5) waitForWritten(recorder, i);
    }
    const auto stats = recorder.stop();
    require(stats.frames == 8 && stats.dropped == 0 && stats.error.empty() && !stats.active,
            "native recording did not drain or report frames");
    const auto durationUs = pts.back() + 66667;
    require(std::abs(stats.actualFps - 8 * 1000000.0 / durationUs) < 0.00001 && stats.actualFps < 6,
            "native actual FPS was replaced by configured FPS");
    verifyDecoded(dir / "native.mp4", pts, luma, durationUs);
    for(const auto& native : released) require(native.expired(), "encoder retained native capture after stop");

    recorder.start(dir / "thirty.mp4", width, height, 30, 23);
    pts.clear(); luma.clear();
    for(int i = 0; i < 31; ++i) {
        pts.push_back((i * 1000000LL + 15) / 30); luma.push_back(30 + (i % 9) * 16);
        recorder.submit(nativeFrame(luma.back()), start + std::chrono::microseconds(pts.back()));
        waitForWritten(recorder, i);
    }
    const auto thirty = recorder.stop();
    require(thirty.frames == 31 && thirty.dropped == 0 && thirty.error.empty(), "30 FPS recording lost source frames");
    require(std::abs(thirty.actualFps - 30) < 0.001, "30 FPS capture timing changed");
    verifyDecoded(dir / "thirty.mp4", pts, luma, pts.back() + 33333);

    for(const auto badOffset : {33333, 20000}) {
        recorder.start(dir / "bad-time.mp4", width, height, 30, 23);
        recorder.submit(nativeFrame(30), start);
        recorder.submit(nativeFrame(46), start + std::chrono::microseconds(33333));
        waitForWritten(recorder, 1);
        recorder.submit(nativeFrame(62), start + std::chrono::microseconds(badOffset));
        const auto bad = recorder.stop();
        require(bad.error.find("timestamp did not advance") != std::string::npos && bad.frames == 2 && !bad.active,
                "duplicate/backward capture timestamp was silently compressed");
        verifyDecoded(dir / "bad-time.mp4", {0, 33333}, {30, 46}, 66666);
    }
    recorder.start(dir / "changed.mp4", width, height, 15, 23);
    recorder.submit(std::make_shared<const std::vector<std::uint8_t>>(48 * 32 * 3 / 2), start);
    require(!recorder.stop().error.empty(), "dimension change ignored");
    recorder.start(dir / "corrupt.mp4", width, height, 15, 23);
    recorder.submit(std::make_shared<const std::vector<std::uint8_t>>(4), start);
    require(!recorder.stop().error.empty(), "invalid native payload silently accepted");
}

void testEncoded(OakNativeRecorder& recorder, const std::filesystem::path& dir) {
    const auto h264 = encodedFixtures(true);
    const auto start = std::chrono::steady_clock::now();
    recorder.start(dir / "encoded.mp4", width, height, 30, 23, OakRecordingEncoding::H264);
    // Begin in the middle of a GOP; the saved MP4 must start at the next IDR.
    for(int i = 1; i < 9; ++i) {
        recorder.submit(h264[i], start + std::chrono::microseconds(i * 33333LL));
        if(i >= 5) waitForWritten(recorder, i - 4);
    }
    const auto stats = recorder.stop();
    require(stats.frames == 5 && stats.error.empty() && stats.dropped == 0, "encoded recording failed IDR synchronization");
    verifyDecoded(dir / "encoded.mp4", {0, 33333, 66666, 99999, 133332}, {94, 110, 126, 142, 158}, 166665);

    recorder.start(dir / "missing-idr.mp4", width, height, 30, 23, OakRecordingEncoding::H264);
    recorder.submit(h264[1], start); recorder.submit(h264[2], start + std::chrono::microseconds(33333));
    const auto missing = recorder.stop();
    require(missing.frames == 0 && !missing.error.empty(), "undecodable P-frames were accepted without IDR");

    recorder.start(dir / "overflow.mp4", width, height, 30, 23, OakRecordingEncoding::H264);
    recorder.submit(std::make_shared<const std::vector<std::uint8_t>>(64 * 1024 * 1024 + 1), start);
    const auto overflow = recorder.stop();
    require(overflow.error.find("queue overflow") != std::string::npos && !overflow.active,
            "encoded byte budget overflow did not stop recording explicitly");

    const auto mjpeg = encodedFixtures(false);
    recorder.start(dir / "encoded.mkv", width, height, 30, 23, OakRecordingEncoding::Mjpeg);
    for(int i = 0; i < 9; ++i) {
        recorder.submit(mjpeg[i], start + std::chrono::microseconds(i * 40000LL)); waitForWritten(recorder, i);
    }
    const auto jpegStats = recorder.stop();
    require(jpegStats.frames == 9 && jpegStats.error.empty() && jpegStats.dropped == 0, "encoded MJPEG recording lost frames");
    verifyDecoded(dir / "encoded.mkv", {0, 40000, 80000, 120000, 160000, 200000, 240000, 280000, 320000},
        {30, 46, 62, 78, 94, 110, 126, 142, 158}, 353333, 1000);
}

std::string shellQuote(const std::filesystem::path& path) {
    std::string result = "'";
    for(const auto c : path.string()) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

void testRotatedMjpeg(OakNativeRecorder& recorder, const std::filesystem::path& dir) {
    const auto path = dir / "rotated.mov";
    const auto rawPath = dir / "upright.gray";
    const std::vector<std::int64_t> pts{0, 40000, 90000, 130007};
    std::vector<cv::Mat> expected;
    std::vector<int> center;
    const auto start = std::chrono::steady_clock::now();
    recorder.start(path, width, height, 25, 23, OakRecordingEncoding::MjpegRotated);
    for(std::size_t i = 0; i < pts.size(); ++i) {
        cv::Mat upright(height, width, CV_8UC1);
        for(int y = 0; y < height; ++y)
            for(int x = 0; x < width; ++x)
                upright.at<std::uint8_t>(y, x) = static_cast<std::uint8_t>(20 + (x * 3 + y * 5 + i * 17) % 180);
        cv::Mat rotated;
        cv::rotate(upright, rotated, cv::ROTATE_90_CLOCKWISE);
        auto jpeg = std::make_shared<std::vector<std::uint8_t>>();
        require(cv::imencode(".jpg", rotated, *jpeg, {cv::IMWRITE_JPEG_QUALITY, 95}), "rotated JPEG fixture failed");
        const auto decoded = cv::imdecode(*jpeg, cv::IMREAD_GRAYSCALE);
        require(decoded.cols == height && decoded.rows == width, "fixture is not rotated coded JPEG");
        center.push_back(decoded.at<std::uint8_t>(width / 2, height / 2));
        cv::Mat restored;
        cv::rotate(decoded, restored, cv::ROTATE_90_COUNTERCLOCKWISE);
        expected.push_back(restored);
        recorder.submit(jpeg, start + std::chrono::microseconds(pts[i]));
        waitForWritten(recorder, i);
    }
    const auto stats = recorder.stop();
    require(stats.frames == pts.size() && stats.dropped == 0 && stats.error.empty(), "rotated MJPEG lost frames");
    require(std::abs(stats.actualFps - pts.size() * 1000000.0 / 170007) < 0.00001,
            "rotated MOV did not retain actual capture timing");
    // The decoder must report actual JPEG dimensions, independently of how a
    // player applies the explicit container orientation.
    verifyDecoded(path, pts, center, 170007, 1, height, width);
    {
        DecodeSession session;
        checked(avformat_open_input(&session.format, path.c_str(), nullptr, nullptr), "rotated MOV cannot open");
        checked(avformat_find_stream_info(session.format, nullptr), "rotated MOV lacks stream information");
        const auto* stream = session.format->streams[0];
        require(stream->codecpar->width == height && stream->codecpar->height == width,
                "MOV coded dimensions were replaced by upright dimensions");
        bool matrixFound = false;
        for(int i = 0; i < stream->nb_side_data; ++i) {
            const auto& side = stream->side_data[i];
            if(side.type != AV_PKT_DATA_DISPLAYMATRIX || side.size < 9 * sizeof(std::int32_t)) continue;
            const auto rotation = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(side.data));
            if(!std::isfinite(rotation) || std::abs(rotation - 90) >= 0.00001) {
                std::cerr << "stored MOV display rotation " << rotation << " matrix:";
                for(int element = 0; element < 9; ++element)
                    std::cerr << ' ' << reinterpret_cast<const std::int32_t*>(side.data)[element];
                std::cerr << '\n';
            }
            require(std::isfinite(rotation) && std::abs(rotation - 90) < 0.00001, "MOV inverse display rotation missing");
            matrixFound = true;
        }
        require(matrixFound, "MOV lacks an explicit display matrix");
    }
    // Exercise the target FFmpeg player's default autorotation, without an
    // explicit transpose filter. Passthrough timestamps prevent frame repeats.
    const auto command = "ffmpeg -nostdin -v error -threads 1 -i " + shellQuote(path) +
        " -map 0:v:0 -vsync 0 -pix_fmt gray -threads 1 -f rawvideo -y " + shellQuote(rawPath);
    require(std::system(command.c_str()) == 0, "FFmpeg failed default oriented MOV decode");
    std::ifstream raw(rawPath, std::ios::binary);
    require(raw.good(), "FFmpeg oriented frames missing");
    const std::vector<std::uint8_t> pixels((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
    require(pixels.size() == pts.size() * width * height, "MOV display dimensions or frame count changed");
    for(std::size_t frame = 0; frame < expected.size(); ++frame)
        for(int y = 0; y < height; ++y)
            for(int x = 0; x < width; ++x)
                require(std::abs(static_cast<int>(pixels[(frame * height + y) * width + x]) -
                    expected[frame].at<std::uint8_t>(y, x)) <= 2,
                    "MOV default playback orientation or complete field of view changed");
}
}

int main() {
    const auto dir = std::filesystem::temp_directory_path() / ("oak-native-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    try {
        OakNativeRecorder recorder;
        testNative(recorder, dir); testEncoded(recorder, dir); testRotatedMjpeg(recorder, dir);
        std::filesystem::remove_all(dir);
        std::cout << "native/device-encoded decode, true FPS/timing, IDR sync, input lifetime, failures and rotated MOV display passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        if(std::getenv("TENCO_KEEP_RECORDING_TEST_DIR")) std::cerr << "test evidence: " << dir << '\n';
        else std::filesystem::remove_all(dir);
        return 1;
    }
}
