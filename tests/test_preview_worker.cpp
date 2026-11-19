#include "oak_preview_worker.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    try {
        std::promise<void> entered, release, finished;
        auto released = release.get_future().share();
        std::vector<int> seen;
        {
            OakPreviewWorker worker([&](const cv::Mat& image, auto) {
                seen.push_back(image.at<unsigned char>(0, 0));
                if(seen.size() == 1) { entered.set_value(); released.wait_for(std::chrono::seconds(3)); }
                else finished.set_value();
            });
            const auto now = std::chrono::steady_clock::now();
            worker.submit(cv::Mat(1, 1, CV_8UC1, cv::Scalar(1)).clone(), now);
            if(entered.get_future().wait_for(std::chrono::seconds(2)) != std::future_status::ready)
                throw std::runtime_error("preview callback did not start");
            // A blocked preview encoder must not block recording ingestion. Only
            // the most recent of these pending preview images may be retained.
            for(int i = 2; i <= 200; ++i)
                worker.submit(cv::Mat(1, 1, CV_8UC1, cv::Scalar(i)).clone(), now);
            release.set_value();
            if(finished.get_future().wait_for(std::chrono::seconds(2)) != std::future_status::ready)
                throw std::runtime_error("latest pending preview did not run");
        }
        if(seen != std::vector<int>{1, 200}) throw std::runtime_error("preview accumulated stale frames");
        std::promise<void> failed;
        OakPreviewWorker worker([&](const cv::Mat&, auto) {
            failed.set_value();
            throw std::runtime_error("expected JPEG failure");
        });
        worker.submit(cv::Mat(1, 1, CV_8UC1), std::chrono::steady_clock::now());
        if(failed.get_future().wait_for(std::chrono::seconds(2)) != std::future_status::ready)
            throw std::runtime_error("failing preview callback did not run");
        bool observed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while(std::chrono::steady_clock::now() < deadline) {
            try { worker.check(); }
            catch(const std::runtime_error&) { observed = true; break; }
            std::this_thread::yield();
        }
        if(!observed) throw std::runtime_error("preview error did not propagate");
        std::cout << "bounded preview slot, independent capture and error propagation passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
