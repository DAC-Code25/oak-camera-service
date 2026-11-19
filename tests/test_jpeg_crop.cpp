#include "oak_jpeg_crop.h"

#include <opencv2/opencv.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <jpeglib.h>
}

namespace {
using Payload = std::shared_ptr<const std::vector<std::uint8_t>>;
void require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}

struct Component {
    JDIMENSION columns = 0, rows = 0;
    int table = 0;
    std::vector<JCOEF> coefficients;
};
struct Coefficients {
    int width = 0, height = 0;
    std::vector<Component> components;
    std::array<std::array<UINT16, DCTSIZE2>, NUM_QUANT_TBLS> quantization{};
};
struct Decoder {
    jpeg_decompress_struct state{};
    jpeg_error_mgr error{};
    Decoder() { state.err = jpeg_std_error(&error); jpeg_create_decompress(&state); }
    ~Decoder() { jpeg_destroy_decompress(&state); }
};

Coefficients coefficients(const Payload& jpeg) {
    Decoder decoder;
    jpeg_mem_src(&decoder.state, jpeg->data(), static_cast<unsigned long>(jpeg->size()));
    require(jpeg_read_header(&decoder.state, TRUE) == JPEG_HEADER_OK, "test JPEG header missing");
    const auto arrays = jpeg_read_coefficients(&decoder.state);
    require(arrays, "test JPEG coefficients missing");
    Coefficients result;
    result.width = decoder.state.image_width; result.height = decoder.state.image_height;
    for(int table = 0; table < NUM_QUANT_TBLS; ++table)
        if(decoder.state.quant_tbl_ptrs[table])
            std::copy_n(decoder.state.quant_tbl_ptrs[table]->quantval, DCTSIZE2, result.quantization[table].begin());
    for(int index = 0; index < decoder.state.num_components; ++index) {
        const auto& info = decoder.state.comp_info[index];
        Component component;
        component.columns = info.width_in_blocks; component.rows = info.height_in_blocks;
        component.table = info.quant_tbl_no;
        for(JDIMENSION row = 0; row < component.rows; ++row) {
            const auto data = (*decoder.state.mem->access_virt_barray)(
                reinterpret_cast<j_common_ptr>(&decoder.state), arrays[index], row, 1, FALSE);
            for(JDIMENSION column = 0; column < component.columns; ++column)
                component.coefficients.insert(component.coefficients.end(), data[0][column], data[0][column] + DCTSIZE2);
        }
        result.components.push_back(std::move(component));
    }
    jpeg_finish_decompress(&decoder.state);
    return result;
}

void verifyCoefficients(const Payload& padded, const Payload& cropped, int width, int height) {
    const auto before = coefficients(padded), after = coefficients(cropped);
    if(after.width != width || after.height != height)
        throw std::runtime_error("crop dimensions " + std::to_string(before.width) + "x" + std::to_string(before.height) +
            " -> " + std::to_string(after.width) + "x" + std::to_string(after.height) +
            "; expected " + std::to_string(width) + "x" + std::to_string(height));
    require(before.components.size() == after.components.size(), "JPEG components changed");
    for(std::size_t component = 0; component < after.components.size(); ++component) {
        const auto& source = before.components[component];
        const auto& target = after.components[component];
        require(source.rows == target.rows && source.columns >= target.columns, "crop changed the visible image grid");
        require(before.quantization[source.table] == after.quantization[target.table], "crop changed JPEG quantization/quality");
        for(JDIMENSION row = 0; row < target.rows; ++row)
            for(JDIMENSION column = 0; column < target.columns; ++column) {
                const auto original = source.coefficients.begin() + (row * source.columns + column) * DCTSIZE2;
                const auto restored = target.coefficients.begin() + (row * target.columns + column) * DCTSIZE2;
                require(std::equal(restored, restored + DCTSIZE2, original), "crop changed visible DCT coefficients");
            }
    }
    const auto fullImage = cv::imdecode(*padded, cv::IMREAD_COLOR);
    const auto croppedImage = cv::imdecode(*cropped, cv::IMREAD_COLOR);
    require(!croppedImage.empty() && croppedImage.cols == width && croppedImage.rows == height,
            "cropped JPEG failed full image decode");
    cv::Mat difference;
    // Chroma upsampling at the final two columns may extend the new visible
    // image edge. The coefficients above verify those edge blocks exactly.
    const cv::Rect visible(0, 0, width - 2, height);
    cv::absdiff(fullImage(visible), croppedImage(visible), difference);
    double maximum = 0;
    cv::minMaxLoc(difference.reshape(1), nullptr, &maximum);
    require(maximum <= 1, "crop shifted, resized or changed visible field of view");
}

Payload image(int width, int height, bool progressive = false) {
    cv::Mat pixels(height, width, CV_8UC3);
    for(int y = 0; y < height; ++y)
        for(int x = 0; x < width; ++x)
            pixels.at<cv::Vec3b>(y, x) = {
                static_cast<unsigned char>((x * 11 + y * 3) % 256),
                static_cast<unsigned char>((x / 7 + y * 9) % 256),
                static_cast<unsigned char>((x * 5 + y / 3) % 256)};
    auto jpeg = std::make_shared<std::vector<std::uint8_t>>();
    require(cv::imencode(".jpg", pixels, *jpeg,
        {cv::IMWRITE_JPEG_QUALITY, 86, cv::IMWRITE_JPEG_PROGRESSIVE, progressive ? 1 : 0}), "test JPEG encode failed");
    return jpeg;
}

template<class Action> void rejected(Action action, const char* message) {
    bool failed = false;
    try { action(); } catch(const std::exception&) { failed = true; }
    require(failed, message);
}
}

int main() {
    try {
        const auto native = image(4064, 96);
        const auto original = *native;
        const auto fast = oakCropJpegRightPadding(native, 4056, 96);
        require(fast->size() == native->size(), "native padding crop unexpectedly re-encoded entropy");
        std::size_t changes = 0;
        for(std::size_t i = 0; i < native->size(); ++i) changes += (*native)[i] != (*fast)[i];
        require(changes > 0 && changes <= 2, "native fast crop changed bytes beyond the SOF width");
        require(*native == original, "crop mutated the capture buffer");
        verifyCoefficients(native, fast, 4056, 96);
        require(oakCropJpegRightPadding(fast, 4056, 96) == fast, "exact-size JPEG was copied");

        const auto padded = image(2048, 96);
        const auto crop = oakCropJpegRightPadding(padded, 2028, 96);
        verifyCoefficients(padded, crop, 2028, 96);

        // Progressive single-component scans need coefficient cropping even if
        // baseline interleaved JPEG could use the SOF-only optimization.
        const auto progressive = image(4064, 96, true);
        verifyCoefficients(progressive, oakCropJpegRightPadding(progressive, 4056, 96), 4056, 96);

        const auto aligned = image(1280, 64);
        require(oakCropJpegRightPadding(aligned, 1280, 64) == aligned, "aligned encoder output was copied");
        rejected([&] { oakCropJpegRightPadding(nullptr, 2028, 96); }, "null JPEG accepted");
        rejected([&] { oakCropJpegRightPadding(padded, 0, 96); }, "invalid desired dimensions accepted");
        rejected([&] { oakCropJpegRightPadding(padded, 2028, 94); }, "wrong encoder height accepted");
        rejected([&] { oakCropJpegRightPadding(padded, 2000, 96); }, "unexpected encoder padding accepted");
        rejected([&] { oakCropJpegRightPadding(std::make_shared<const std::vector<std::uint8_t>>(4, 0), 2028, 96); },
            "invalid JPEG markers accepted");
        auto corrupt = std::make_shared<std::vector<std::uint8_t>>(padded->begin(), padded->begin() + padded->size() / 2);
        corrupt->push_back(0xff); corrupt->push_back(0xd9);
        for(int i = 0; i < 3; ++i)
            rejected([&] { oakCropJpegRightPadding(corrupt, 2028, 96); }, "truncated JPEG entropy accepted");
        // A fatal libjpeg input error must clean up and leave subsequent frames usable.
        verifyCoefficients(padded, oakCropJpegRightPadding(padded, 2028, 96), 2028, 96);
        std::cout << "MJPEG exact dimensions, SOF fast path, lossless coefficients, full field of view and failures passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
