#include "oak_preview_worker.h"
#include <utility>

OakPreviewWorker::OakPreviewWorker(Callback callback)
    : callback_(std::move(callback)), thread_(&OakPreviewWorker::run, this) {}

OakPreviewWorker::~OakPreviewWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_.release();
        cv_.notify_one();
    }
    thread_.join();
}

void OakPreviewWorker::submit(const cv::Mat& image, std::chrono::steady_clock::time_point capturedAt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(error_) std::rethrow_exception(error_);
    pending_ = image;
    pendingAt_ = capturedAt;
    cv_.notify_one();
}

void OakPreviewWorker::check() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(error_) std::rethrow_exception(error_);
}

void OakPreviewWorker::run() {
    try {
        for(;;) {
            cv::Mat image;
            std::chrono::steady_clock::time_point capturedAt;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
                if(stopping_) return;
                image = std::move(pending_);
                capturedAt = pendingAt_;
            }
            callback_(image, capturedAt);
        }
    } catch(...) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::current_exception();
    }
}
