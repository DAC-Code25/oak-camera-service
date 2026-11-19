#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// Remove only the encoder's right padding to a 32-pixel width boundary.
// The visible JPEG coefficients and quantization tables are preserved.
std::shared_ptr<const std::vector<std::uint8_t>> oakCropJpegRightPadding(
    std::shared_ptr<const std::vector<std::uint8_t>> jpeg, int width, int height);
