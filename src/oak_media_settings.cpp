#include "oak_media_settings.h"
#include <cmath>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <memory>
#include <stdexcept>

namespace {
void keys(const Json::Value& v, const std::set<std::string>& allowed) {
    if(!v.isObject()) throw std::invalid_argument("settings must be a JSON object");
    for(const auto& k : v.getMemberNames())
        if(!allowed.count(k)) throw std::invalid_argument("unknown settings field: " + k);
}
double number(const Json::Value& v, const char* key) {
    if(!v[key].isNumeric() || !std::isfinite(v[key].asDouble()))
        throw std::invalid_argument(std::string("invalid settings number: ") + key);
    return v[key].asDouble();
}
OakOutputSettings output(const Json::Value& v) {
    keys(v, {"width", "height", "fps"});
    const auto w = number(v, "width"), h = number(v, "height"), f = number(v, "fps");
    if(w < 0 || w > 8192 || h < 0 || h > 8192 || std::floor(w) != w || std::floor(h) != h)
        throw std::invalid_argument("dimensions must be bounded integers");
    return {static_cast<int>(w), static_cast<int>(h), f};
}
Json::Value outputJson(const OakOutputSettings& s) {
    Json::Value v; v["width"] = s.width; v["height"] = s.height; v["fps"] = s.fps; return v;
}
}
Json::Value oakSettingsJson(const OakMediaSettings& s) {
    Json::Value v; v["sensorFps"] = s.sensorFps;
    v["preview"] = outputJson(s.preview); v["photo"] = outputJson(s.photo);
    v["recording"] = outputJson(s.recording); return v;
}
OakMediaSettings oakSettingsFromJson(const Json::Value& v) {
    keys(v, {"sensorFps", "preview", "photo", "recording"});
    OakMediaSettings s; s.sensorFps = number(v, "sensorFps");
    s.preview = output(v["preview"]); s.photo = output(v["photo"]); s.recording = output(v["recording"]);
    validateOakMediaSettings(s); return s;
}
void validateOakMediaSettings(const OakMediaSettings& s, const OakCaptureInfo* info) {
    const double maximum = info ? info->maxFps : 30;
    const double minimum = info ? info->minFps : 1.42;
    if(!std::isfinite(s.sensorFps) || s.sensorFps < minimum || s.sensorFps > maximum)
        throw std::invalid_argument("sensorFps is outside the native sensor range");
    const int maxW = info ? info->nativeWidth : 4208, maxH = info ? info->nativeHeight : 3120;
    for(const auto* o : {&s.preview, &s.photo, &s.recording}) {
        const bool native = o == &s.photo || o == &s.recording;
        if(!std::isfinite(o->fps) || o->fps < 1 || o->fps > s.sensorFps ||
           ((o->width == 0 || o->height == 0) && !(native && o->width == 0 && o->height == 0)) ||
           o->width < 0 || o->height < 0 || o->width > maxW || o->height > maxH ||
           (o->width > 0 && (o->width < 32 || o->height < 32)) || o->width % 2 || o->height % 2)
            throw std::invalid_argument("invalid output dimensions/FPS; use even sizes within the native image");
    }
    if(s.preview.width > 1920 || s.preview.height > 1080)
        throw std::invalid_argument("preview exceeds 1920x1080");
}
Json::Value oakParseJson(const std::string& text) {
    Json::CharReaderBuilder b; b["rejectDupKeys"] = true; b["failIfExtra"] = true;
    b["allowComments"] = false; b["collectComments"] = false;
    b["allowTrailingCommas"] = false;
    Json::Value v; std::string error; auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
    if(text.empty() || !reader->parse(text.data(), text.data()+text.size(), &v, &error))
        throw std::invalid_argument("invalid JSON body");
    return v;
}
std::string oakJsonText(const Json::Value& v) {
    Json::StreamWriterBuilder b; b["indentation"] = ""; return Json::writeString(b, v);
}
void oakPersistSettings(const std::filesystem::path& path, const OakMediaSettings& s, std::uint64_t revision,
                        const OakMediaSettings* fallback) {
    if(path.empty()) return;
    Json::Value v; v["schemaVersion"] = 1; v["revision"] = std::to_string(revision); v["settings"] = oakSettingsJson(s);
    if(fallback) v["fallbackSettings"] = oakSettingsJson(*fallback);
    const auto data = oakJsonText(v);
    auto tmp = path; tmp += ".pending";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(fd < 0) throw std::runtime_error("cannot create settings file: " + std::string(std::strerror(errno)));
    std::string error;
    std::size_t done = 0;
    while(done < data.size()) {
        const auto n = ::write(fd, data.data()+done, data.size()-done);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) { error = std::strerror(errno); break; }
        done += n;
    }
    if(error.empty() && ::fsync(fd) < 0) error = std::strerror(errno);
    if(::close(fd) < 0 && error.empty()) error = std::strerror(errno);
    if(error.empty() && ::rename(tmp.c_str(), path.c_str()) < 0) error = std::strerror(errno);
    if(!error.empty()) { ::unlink(tmp.c_str()); throw std::runtime_error("settings persistence failed: " + error); }
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if(dir >= 0) { ::fsync(dir); ::close(dir); }
}
