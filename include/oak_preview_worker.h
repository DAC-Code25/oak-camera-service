#pragma once

#include <opencv2/core.hpp>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

// Preview is disposable: keep one pending image so JPEG work never delays
// draining the ordered recording stream. Destruction joins the callback.
class OakPreviewWorker {
public:
    using Callback = std::function<void(const cv::Mat&, std::chrono::steady_clock::time_point)>;
    explicit OakPreviewWorker(Callback callback);
    ~OakPreviewWorker();
    void submit(const cv::Mat& image, std::chrono::steady_clock::time_point capturedAt);
    void check() const;
private:
    void run();
    Callback callback_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    cv::Mat pending_;
    std::chrono::steady_clock::time_point pendingAt_{};
    std::exception_ptr error_;
    bool stopping_ = false;
    std::thread thread_;
};
