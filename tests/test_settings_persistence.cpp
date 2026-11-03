#include "oak_camera_device.h"
#include <cerrno>
#include <atomic>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
void require(bool value, const char* reason) { if(!value) throw std::runtime_error(reason); }
Json::Value read(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return oakParseJson({std::istreambuf_iterator<char>(stream), {}});
}
template<class F> void waitFor(F predicate, const char* reason) {
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) {
        if(std::chrono::steady_clock::now() > until) throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path()/("oak-persistence-"+std::to_string(getpid()));
    try {
        std::filesystem::create_directories(directory);
        const auto path = directory/"settings.json";
        OakMediaSettings initial;
        initial.preview = {48,32,15}; initial.recording = {96,64,30};
        require(oakPersistSettings(path, initial, 1).confirmed, "normal directory fsync failed");
        auto changed = initial; changed.photo = {64,48,2};
        OakSettingsDirectoryOps failedOpen{
            [](const auto&) { errno = EACCES; return -1; }, [](int) { return 0; }};
        auto result = oakPersistSettings(path, changed, 2, &initial, &failedOpen);
        require(!result.confirmed && result.error.find("directory open failed") != std::string::npos,
                "post-rename open failure claimed durability");
        require(read(path)["revision"] == "2" && read(path)["fallbackSettings"] == oakSettingsJson(initial),
                "post-rename failure lost visible revision or fallback");
        OakSettingsDirectoryOps failedSync{
            [](const auto& p) { return ::open(p.c_str(), O_RDONLY|O_DIRECTORY); },
            [](int) { errno = EIO; return -1; }};
        result = oakPersistSettings(path, initial, 3, nullptr, &failedSync);
        require(!result.confirmed && result.error.find("directory fsync failed") != std::string::npos &&
                read(path)["revision"] == "3", "post-rename fsync failure not reported accurately");
        bool threw = false;
        try { oakPersistSettings(directory/"missing"/"settings.json", initial, 4); }
        catch(const std::exception&) { threw = true; }
        require(threw && read(path)["revision"] == "3", "pre-rename error changed the existing revision");

        // Exercise operation state, not just the persistence helper. A visible
        // replacement must advance memory even when directory durability fails.
        OakCameraConfig config;
        config.previewWidth = 48; config.previewHeight = 32; config.media = initial;
        config.settingsFile = path; config.snapshotDir = directory; config.recordingDir = directory;
        auto failSync = std::make_shared<std::atomic<bool>>(true);
        config.settingsDirectoryOps = std::make_shared<OakSettingsDirectoryOps>(OakSettingsDirectoryOps{
            [](const auto& p) { return ::open(p.c_str(), O_RDONLY|O_DIRECTORY); },
            [failSync](int descriptor) { if(failSync->load()) { errno = EIO; return -1; } return ::fsync(descriptor); }});
        OakCameraDevice camera(config); camera.start();
        waitFor([&] { return camera.settings()["applyState"] == "applied"; }, "camera did not apply initial settings");
        auto current = camera.settings();
        require(!current["persisted"].asBool() && current["persistenceState"] == "unconfirmed" &&
                !current["persistenceError"].asString().empty(), "startup durability uncertainty hidden");
        Json::Value request;
        request["schemaVersion"] = 1; request["expectedRevision"] = current["revision"];
        request["settings"] = oakSettingsJson(changed);
        auto pending = camera.updateSettings(request);
        require(pending["revision"] == "4" && !pending["persisted"].asBool() &&
                pending["persistenceState"] == "unconfirmed", "renamed update left stale in-memory revision");
        waitFor([&] { return camera.settings()["applyState"] == "applied"; }, "updated camera settings not applied");
        current = camera.settings();
        require(current["activeSettings"] == oakSettingsJson(changed) && !current["persisted"].asBool(),
                "uncertain persistence confused runtime application");

        // Failed hardware application still restores the known-good fallback.
        auto rejected = changed; rejected.recording.width = 80;
        request["expectedRevision"] = current["revision"]; request["settings"] = oakSettingsJson(rejected);
        camera.updateSettings(request);
        waitFor([&] { const auto v = camera.settings(); return v["applyState"] == "rejected" && v["cameraConnected"].asBool(); },
                "uncertain candidate lost rollback recovery");
        current = camera.settings();
        require(current["settings"] == oakSettingsJson(changed) && !current["persisted"].asBool() &&
                read(path)["settings"] == oakSettingsJson(changed), "rollback revision/durability not reflected");
        const auto beforeRetry = camera.status();
        failSync->store(false);
        request["expectedRevision"] = current["revision"]; request["settings"] = oakSettingsJson(changed);
        const auto retried = camera.updateSettings(request);
        require(retried["persisted"].asBool() && retried["persistenceState"] == "confirmed" &&
                retried["persistenceError"].asString().empty() && retried["revision"] == current["revision"] &&
                retried["activeRevision"] == current["activeRevision"] && retried["applyState"] == current["applyState"] &&
                camera.status().reconnectCount == beforeRetry.reconnectCount && camera.status().cameraConnected,
                "same-value durability retry changed revision, restarted camera or failed to confirm persistence");
        camera.stop();
        std::filesystem::remove_all(directory);
        std::cout << "pre-rename failure, directory open/fsync uncertainty, state alignment and rollback passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n'; std::filesystem::remove_all(directory); return 1;
    }
}
