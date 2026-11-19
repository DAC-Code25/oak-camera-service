#include "oak_jpeg_crop.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

extern "C" {
#include <jpeglib.h>
}

namespace {
struct JpegError {
    jpeg_error_mgr manager;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void failJpeg(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegError*>(common->err);
    (*common->err->format_message)(common, error->message);
    std::longjmp(error->jump, 1);
}
void reportJpeg(j_common_ptr common, int level) {
    // A truncated/corrupt input must not become a nominally successful recording.
    if(level < 0) failJpeg(common);
}

struct JpegSession {
    JpegError error{};
    jpeg_decompress_struct source{};
    jpeg_compress_struct destination{};
    bool sourceCreated = false, destinationCreated = false;
    unsigned char* output = nullptr;
    unsigned long outputSize = 0;

    ~JpegSession() {
        if(destinationCreated) jpeg_destroy_compress(&destination);
        if(sourceCreated) jpeg_destroy_decompress(&source);
        std::free(output);
    }
};

// Locate the baseline SOF width while checking marker lengths. Entropy bytes
// need no changes when the interleaved MCU grid remains exactly the same.
std::size_t baselineWidthOffset(const std::vector<std::uint8_t>& jpeg) {
    std::size_t position = 2;
    while(position < jpeg.size()) {
        if(jpeg[position++] != 0xff) throw std::runtime_error("invalid JPEG marker before scan");
        while(position < jpeg.size() && jpeg[position] == 0xff) ++position;
        if(position == jpeg.size()) break;
        const auto marker = jpeg[position++];
        if(marker == 0xda || marker == 0xd9) break;
        if(marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) continue;
        if(position + 2 > jpeg.size()) break;
        const auto length = static_cast<std::size_t>(jpeg[position]) * 256 + jpeg[position + 1];
        if(length < 2 || length > jpeg.size() - position)
            throw std::runtime_error("truncated JPEG marker before scan");
        if(marker == 0xc0 && length >= 8) return position + 5;
        position += length;
    }
    return 0;
}
}

std::shared_ptr<const std::vector<std::uint8_t>> oakCropJpegRightPadding(
    std::shared_ptr<const std::vector<std::uint8_t>> jpeg, int width, int height) {
    if(!jpeg) throw std::invalid_argument("MJPEG recording frame is null");
    if(width <= 0 || height <= 0 || width > JPEG_MAX_DIMENSION || height > JPEG_MAX_DIMENSION)
        throw std::invalid_argument("invalid MJPEG recording dimensions");
    if(jpeg->size() < 4 || (*jpeg)[0] != 0xff || (*jpeg)[1] != 0xd8 ||
       (*jpeg)[jpeg->size() - 2] != 0xff || jpeg->back() != 0xd9 ||
       jpeg->size() > std::numeric_limits<unsigned long>::max())
        throw std::runtime_error("invalid encoded MJPEG recording frame");

    // The heap session is created before setjmp. No nontrivial C++ object is
    // constructed across a libjpeg call that may longjmp, and cleanup stays in
    // this scope, including partially allocated libjpeg memory on fatal errors.
    auto session = std::make_unique<JpegSession>();
    session->source.err = jpeg_std_error(&session->error.manager);
    session->error.manager.error_exit = failJpeg;
    session->error.manager.emit_message = reportJpeg;
    if(setjmp(session->error.jump))
        throw std::runtime_error(std::string("MJPEG padding crop: ") + session->error.message);

    session->sourceCreated = true;
    jpeg_create_decompress(&session->source);
    jpeg_mem_src(&session->source, jpeg->data(), static_cast<unsigned long>(jpeg->size()));
    if(jpeg_read_header(&session->source, TRUE) != JPEG_HEADER_OK)
        throw std::runtime_error("MJPEG recording frame lacks an image header");
    const auto encodedWidth = session->source.image_width;
    if(session->source.image_height != static_cast<JDIMENSION>(height))
        throw std::runtime_error("MJPEG recording height changed");
    if(encodedWidth == static_cast<JDIMENSION>(width)) return jpeg;
    const auto paddedWidth = static_cast<JDIMENSION>((width + 31) / 32 * 32);
    if(encodedWidth != paddedWidth)
        throw std::runtime_error("MJPEG recording width is not the expected right padding");

    int maxSampling = 1;
    for(int component = 0; component < session->source.num_components; ++component)
        if(session->source.comp_info[component].h_samp_factor > maxSampling)
            maxSampling = session->source.comp_info[component].h_samp_factor;
    const auto mcuWidth = static_cast<JDIMENSION>(DCTSIZE * maxSampling);
    const auto widthOffset = baselineWidthOffset(*jpeg);
    if(widthOffset && !session->source.progressive_mode &&
       session->source.comps_in_scan == session->source.num_components &&
       (encodedWidth + mcuWidth - 1) / mcuWidth == (width + mcuWidth - 1) / mcuWidth) {
        auto cropped = std::make_shared<std::vector<std::uint8_t>>(*jpeg);
        (*cropped)[widthOffset] = static_cast<std::uint8_t>(width >> 8);
        (*cropped)[widthOffset + 1] = static_cast<std::uint8_t>(width & 255);
        return cropped;
    }

    const auto coefficients = jpeg_read_coefficients(&session->source);
    if(!coefficients) throw std::runtime_error("MJPEG coefficients are unavailable");
    session->destination.err = &session->error.manager;
    session->destinationCreated = true;
    jpeg_create_compress(&session->destination);
    jpeg_mem_dest(&session->destination, &session->output, &session->outputSize);
    jpeg_copy_critical_parameters(&session->source, &session->destination);
    session->destination.image_width = static_cast<JDIMENSION>(width);
    session->destination.image_height = static_cast<JDIMENSION>(height);
#if JPEG_LIB_VERSION >= 70
    // The libjpeg 7/8 coefficient API writes SOF from jpeg_width/jpeg_height;
    // jpeg_copy_critical_parameters initially sets them to the padded source.
    session->destination.jpeg_width = static_cast<JDIMENSION>(width);
    session->destination.jpeg_height = static_cast<JDIMENSION>(height);
#endif
    // Source arrays may be wider; the compressor reads only the left destination
    // MCU columns. No inverse DCT, resampling or quantization takes place.
    jpeg_write_coefficients(&session->destination, coefficients);
    jpeg_finish_compress(&session->destination);
    jpeg_finish_decompress(&session->source);
    return std::make_shared<const std::vector<std::uint8_t>>(
        session->output, session->output + session->outputSize);
}
