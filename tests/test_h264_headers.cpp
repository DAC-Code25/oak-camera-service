#include "oak_h264_headers.h"
#include <iostream>
#include <stdexcept>

using Bytes = std::vector<std::uint8_t>;

Bytes joined(std::initializer_list<Bytes> packets) {
    Bytes result;
    for(const auto& packet : packets) result.insert(result.end(), packet.begin(), packet.end());
    return result;
}

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const Bytes sps{0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f};
        const Bytes pps{0, 0, 0, 1, 0x68, 0xce, 0x01};
        const Bytes ppsShort{0, 0, 1, 0x68, 0xce, 0x01};
        const Bytes p{0, 0, 1, 0x41, 0x11};
        const Bytes idr{0, 0, 0, 1, 0x65, 0x22};
        auto payload = [](const Bytes& bytes) { return std::make_shared<const Bytes>(bytes); };
        OakH264Headers headers;
        const auto startup = payload(joined({sps, ppsShort}));
        require(headers.prepare(startup) == startup, "header-only packet changed");
        const auto predicted = payload(p);
        require(headers.prepare(predicted) == predicted, "parameter sets inserted into P frame");
        require(*headers.prepare(payload(idr)) == joined({sps, pps, idr}), "later IDR lost startup headers");
        const auto repeated = payload(joined({sps, ppsShort, idr}));
        require(headers.prepare(repeated) == repeated, "existing headers duplicated");
        const Bytes changedSps{0, 0, 0, 1, 0x67, 0x4d, 0x00, 0x20};
        headers.prepare(payload(changedSps));
        require(*headers.prepare(payload(idr)) == joined({changedSps, pps, idr}), "new SPS not retained");
        require(*headers.prepare(payload(joined({changedSps, idr}))) == joined({changedSps, pps, idr}),
                "missing PPS was not inserted after current SPS");
        require(*headers.prepare(payload(joined({pps, idr}))) == joined({changedSps, pps, idr}),
                "missing SPS was not inserted before current PPS");
        OakH264Headers separate;
        separate.prepare(payload(sps));
        const auto noPps = payload(idr);
        require(separate.prepare(noPps) == noPps, "incomplete parameter sets fabricated");
        separate.prepare(payload(ppsShort));
        require(*separate.prepare(payload(idr)) == joined({sps, pps, idr}), "separate parameter packets lost");
        std::cout << "startup H264 headers, later IDR, P frames, header updates and separate packets passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
