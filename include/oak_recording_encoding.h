#pragma once

enum class OakRecordingEncoding {
    Yuv420p,
    H264,
    Mjpeg,
    // JPEG pixels are rotated clockwise by 90 degrees on the device. MOV
    // records their actual coded dimensions and an inverse display transform.
    MjpegRotated
};
