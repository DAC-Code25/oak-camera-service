#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// A gated encoder may emit SPS/PPS only once, before the first recording starts.
// Retain them independently of each writer and attach them to later IDR frames.
class OakH264Headers {
public:
    using Payload = std::shared_ptr<const std::vector<std::uint8_t>>;

    Payload prepare(Payload bytes) {
        if(!bytes) return bytes;
        bool hasSps = false, hasPps = false, idr = false;
        std::size_t firstIdr = bytes->size();
        auto prefix = [&](std::size_t at) -> std::size_t {
            if(at + 3 <= bytes->size() && (*bytes)[at] == 0 && (*bytes)[at + 1] == 0) {
                if((*bytes)[at + 2] == 1) return 3;
                if(at + 4 <= bytes->size() && (*bytes)[at + 2] == 0 && (*bytes)[at + 3] == 1) return 4;
            }
            return 0;
        };
        for(std::size_t position = 0; position < bytes->size();) {
            const auto length = prefix(position);
            if(!length) { ++position; continue; }
            const auto first = position + length;
            auto end = first;
            while(end < bytes->size() && !prefix(end)) ++end;
            if(first < end) {
                const auto type = (*bytes)[first] & 31;
                if(type == 7 || type == 8) {
                    auto& stored = type == 7 ? sps_ : pps_;
                    stored.assign(bytes->begin() + first, bytes->begin() + end);
                    if(type == 7) hasSps = true;
                    else hasPps = true;
                }
                if(type == 5) { idr = true; if(firstIdr == bytes->size()) firstIdr = position; }
            }
            position = end;
        }
        if(!idr || (hasSps && hasPps) || sps_.empty() || pps_.empty()) return bytes;
        auto result = std::make_shared<std::vector<std::uint8_t>>();
        result->reserve(bytes->size() + sps_.size() + pps_.size() + 8);
        auto appendParameter = [&](const auto& parameter) {
            result->insert(result->end(), {0, 0, 0, 1});
            result->insert(result->end(), parameter.begin(), parameter.end());
        };
        if(!hasSps) appendParameter(sps_);
        if(!hasPps && hasSps) {
            // Keep an already present SPS before the inserted PPS.
            result->insert(result->end(), bytes->begin(), bytes->begin() + firstIdr);
            appendParameter(pps_);
            result->insert(result->end(), bytes->begin() + firstIdr, bytes->end());
        } else {
            if(!hasPps) appendParameter(pps_);
            result->insert(result->end(), bytes->begin(), bytes->end());
        }
        return result;
    }

private:
    std::vector<std::uint8_t> sps_, pps_;
};
