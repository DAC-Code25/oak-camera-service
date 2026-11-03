#pragma once
#include <json/json.h>
#include <filesystem>
#include <string>
#include <functional>
#include "oak_capture_source.h"

struct OakOutputSettings {
    int width = 0, height = 0;
    double fps = 1;
};
struct OakMediaSettings {
    double sensorFps = 30;
    OakOutputSettings preview{960, 540, 15};
    OakOutputSettings photo{0, 0, 2}; // 0x0 means native. FPS refreshes the photo buffer.
    OakOutputSettings recording{1280, 720, 30};
};
Json::Value oakSettingsJson(const OakMediaSettings& settings);
OakMediaSettings oakSettingsFromJson(const Json::Value& value);
void validateOakMediaSettings(const OakMediaSettings& settings, const OakCaptureInfo* info = nullptr);
Json::Value oakParseJson(const std::string& text);
std::string oakJsonText(const Json::Value& value);
struct OakSettingsPersistence {
    bool confirmed = false;
    std::string error;
};
// Optional directory operations allow deterministic post-rename fault tests.
struct OakSettingsDirectoryOps {
    std::function<int(const std::filesystem::path&)> open;
    std::function<int(int)> sync;
};
// Throws only before replacement. After rename the new revision is visible;
// a directory failure returns unconfirmed durability and must not undo memory.
OakSettingsPersistence oakPersistSettings(const std::filesystem::path& path, const OakMediaSettings& settings,
                        std::uint64_t revision, const OakMediaSettings* fallback = nullptr,
                        const OakSettingsDirectoryOps* directoryOps = nullptr);
