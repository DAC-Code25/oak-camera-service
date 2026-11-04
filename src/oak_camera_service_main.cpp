#include "oak_camera_device.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cctype>

namespace {

struct ClientConnection {
    std::mutex mutex;
    int fd = -1;
    std::atomic<bool> done{false};
    std::thread worker;
};

std::atomic<bool> gRunning{true};
std::atomic<int> gMediaStreams{0};

struct StreamSlot {
    bool acquired = false;
    StreamSlot() { acquired = gMediaStreams.fetch_add(1) < 4; if(!acquired) --gMediaStreams; }
    ~StreamSlot() { if(acquired) --gMediaStreams; }
};

void signalHandler(int) {
    gRunning.store(false);
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

HttpRequest parseRequestLine(const std::string& requestText) {
    std::istringstream stream(requestText);
    HttpRequest request;
    std::string version;
    stream >> request.method >> request.path >> version;
    return request;
}

bool sendAll(int fd, const void* data, std::size_t size, int flags = 0) {
    const auto* ptr = static_cast<const char*>(data);
    std::size_t sent = 0;
    while(sent < size) {
        const auto written = ::send(fd, ptr + sent, size - sent, MSG_NOSIGNAL | flags);
        if(written < 0 && errno == EINTR) continue;
        if(written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool sendTextResponse(int fd, int statusCode, const std::string& statusText, const std::string& contentType, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << ' ' << statusText << "\r\n"
        << "Connection: close\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    const auto text = oss.str();
    return sendAll(fd, text.data(), text.size());
}

std::string makeJsonResponse(OakCameraDevice& camera, const OakStatus& status) {
    (void)camera;
    return oakStatusToJson(status);
}

void handleMjpegStream(int fd, OakCameraDevice& camera, const OakCameraConfig& config) {
    StreamSlot slot;
    if(!slot.acquired) {
        sendTextResponse(fd, 503, "Service Unavailable", "application/json", "{\"success\":false,\"message\":\"实时媒体最多同时连接4个客户端\"}");
        return;
    }
    std::uint64_t lastFrameId = 0;
    std::vector<std::uint8_t> jpeg;
    if(!camera.waitForNextJpeg(0, lastFrameId, jpeg, std::chrono::milliseconds(1500))) {
        sendTextResponse(fd, 503, "Service Unavailable", "application/json", oakStatusToJson(camera.status()));
        return;
    }
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
           << "Connection: close\r\n"
           << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
           << "Pragma: no-cache\r\n"
           << "Content-Type: multipart/x-mixed-replace; boundary=" << config.mjpegBoundary << "\r\n\r\n";
    const auto headerText = header.str();
    if(!sendAll(fd, headerText.data(), headerText.size())) {
        return;
    }

    while(gRunning.load()) {
        std::ostringstream partHeader;
        partHeader << "--" << config.mjpegBoundary << "\r\n"
                   << "Content-Type: image/jpeg\r\n"
                   << "Content-Length: " << jpeg.size() << "\r\n\r\n";
        const auto partText = partHeader.str();
        // Flush once the whole multipart frame has been submitted. MSG_MORE
        // avoids separate tiny TCP packets for the header and CRLF without
        // buffering a second JPEG or delaying the next frame.
        if(!sendAll(fd, partText.data(), partText.size(), MSG_MORE)) {
            break;
        }
        if(!sendAll(fd, jpeg.data(), jpeg.size(), MSG_MORE)) {
            break;
        }
        if(!sendAll(fd, "\r\n", 2)) {
            break;
        }
        std::uint64_t currentFrameId = lastFrameId;
        jpeg.clear();
        while(gRunning.load() && jpeg.empty() && camera.status().cameraConnected) {
            camera.waitForNextJpeg(lastFrameId, currentFrameId, jpeg,
                                  std::chrono::milliseconds(config.frameFreshnessMs));
        }
        if(jpeg.empty()) break;
        lastFrameId = currentFrameId;
    }
}

void depthUnavailable(int fd, OakCameraDevice& camera) {
    auto status = camera.depthStatus(); status["success"] = false;
    sendTextResponse(fd,503,"Service Unavailable","application/json",oakJsonText(status));
}

std::string depthImageHeaders(const OakDepthImage& image) {
    std::ostringstream text;
    text << "X-Frame-Id: " << image.frameId << "\r\n"
         << "X-Captured-Monotonic-Ms: " << image.capturedMonotonicMs << "\r\n"
         << "X-Depth-Unit: millimeter\r\nX-Depth-Invalid-Value: 0\r\n"
         << "X-Depth-Reference: rectified_left_camera\r\n";
    return text.str();
}

void handleDepth(int fd, OakCameraDevice& camera, const OakCameraConfig& config, bool png, bool stream) {
    StreamSlot slot;
    if(!slot.acquired) {
        sendTextResponse(fd,503,"Service Unavailable","application/json","{\"success\":false,\"message\":\"实时媒体最多同时连接4个客户端\"}"); return;
    }
    OakDepthImage image;
    const auto depthWait = std::chrono::milliseconds(std::max(1000, 2000/config.depthFps));
    if(!camera.waitForDepthImage(png, 0, image, depthWait)) { depthUnavailable(fd,camera); return; }
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: ";
    if(stream) header << "multipart/x-mixed-replace; boundary=" << config.mjpegBoundary << "\r\n\r\n";
    else header << (png ? "image/png" : "image/jpeg") << "\r\nContent-Length: " << image.bytes->size() << "\r\n"
                << depthImageHeaders(image) << "\r\n";
    const auto text = header.str();
    if(!sendAll(fd,text.data(),text.size(),MSG_MORE)) return;
    if(!stream) { sendAll(fd,image.bytes->data(),image.bytes->size()); return; }
    while(gRunning.load()) {
        std::ostringstream part;
        part << "--" << config.mjpegBoundary << "\r\nContent-Type: image/jpeg\r\nContent-Length: " << image.bytes->size()
             << "\r\n" << depthImageHeaders(image) << "\r\n";
        const auto partText = part.str();
        if(!sendAll(fd,partText.data(),partText.size(),MSG_MORE) ||
           !sendAll(fd,image.bytes->data(),image.bytes->size(),MSG_MORE) || !sendAll(fd,"\r\n",2)) break;
        const auto previous = image.frameId;
        if(!camera.waitForDepthImage(false,previous,image,depthWait)) break;
    }
}

void handleClient(int fd, OakCameraDevice& camera, const OakCameraConfig& config) {
    try {
    std::string requestBuffer;
    char chunk[4096]; std::size_t end;
    while((end = requestBuffer.find("\r\n\r\n")) == std::string::npos) {
        const auto n = ::recv(fd, chunk, sizeof(chunk), 0);
        if(n <= 0) return;
        requestBuffer.append(chunk, n);
        if(requestBuffer.size() > 24576) throw std::invalid_argument("HTTP request too large");
    }
    if(end > 8192) throw std::invalid_argument("HTTP header too large");
    auto request = parseRequestLine(requestBuffer);
    std::size_t length = 0; bool hasLength = false;
    std::istringstream headers(requestBuffer.substr(0,end)); std::string line; std::getline(headers,line);
    while(std::getline(headers,line)) {
        const auto colon = line.find(':'); if(colon == std::string::npos) continue;
        auto name = line.substr(0,colon);
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return std::tolower(c);});
        if(name == "transfer-encoding") throw std::invalid_argument("chunked HTTP bodies are not supported");
        if(name == "content-length") {
            if(hasLength) throw std::invalid_argument("duplicate Content-Length");
            auto value = line.substr(colon+1); value.erase(0,value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r")+1);
            if(value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size()>6)
                throw std::invalid_argument("invalid Content-Length");
            length = std::stoul(value); hasLength = true;
            if(length > 16384) throw std::invalid_argument("HTTP body exceeds 16 KiB");
        }
    }
    while(requestBuffer.size()-end-4 < length) {
        const auto n = ::recv(fd,chunk,std::min(sizeof(chunk),length-(requestBuffer.size()-end-4)),0);
        if(n <= 0) throw std::invalid_argument("incomplete HTTP body");
        requestBuffer.append(chunk,n);
    }
    request.body = requestBuffer.substr(end+4,length);
    if(request.method == "GET" && request.path == "/camera/status") {
        const auto status = camera.status();
        auto v = oakParseJson(makeJsonResponse(camera, status)); v["mediaConfiguration"] = camera.settings();
        v["depth"] = camera.depthStatus();
        sendTextResponse(fd, 200, "OK", "application/json", oakJsonText(v));
    } else if(request.method == "GET" && request.path == "/camera/depth/status") {
        sendTextResponse(fd,200,"OK","application/json",oakJsonText(camera.depthStatus()));
    } else if(request.method == "GET" && request.path == "/camera/depth/image.png") {
        handleDepth(fd,camera,config,true,false);
    } else if(request.method == "GET" && request.path == "/camera/depth/image.jpg") {
        handleDepth(fd,camera,config,false,false);
    } else if(request.method == "GET" && request.path == "/camera/depth/stream.mjpeg") {
        handleDepth(fd,camera,config,false,true);
    } else if(request.method == "GET" && request.path == "/camera/capabilities") {
        sendTextResponse(fd,200,"OK","application/json",oakJsonText(camera.capabilities()));
    } else if(request.method == "GET" && request.path == "/camera/settings") {
        sendTextResponse(fd,200,"OK","application/json",oakJsonText(camera.settings()));
    } else if(request.method == "PUT" && request.path == "/camera/settings") {
        const auto result = camera.updateSettings(oakParseJson(request.body));
        std::clog << "相机媒体参数更新已受理，修订号=" << result["revision"].asString() << std::endl;
        const bool pending = result["applyState"] == "pending";
        sendTextResponse(fd,pending?202:200,pending?"Accepted":"OK","application/json",oakJsonText(result));
    } else if(request.method == "POST" && request.path == "/camera/photo") {
        const auto status = camera.savePhoto();
        std::clog << "拍照请求" << (status.success ? "完成" : "失败") << (status.success ? "" : ": "+status.message) << std::endl;
        sendTextResponse(fd, status.success ? 200 : 500, status.success ? "OK" : "Internal Server Error", "application/json", makeJsonResponse(camera, status));
    } else if(request.method == "POST" && request.path == "/camera/record/start") {
        const auto status = camera.startRecording();
        std::clog << "开始录像请求" << (status.success ? "完成" : "失败") << (status.success ? "" : ": "+status.message) << std::endl;
        sendTextResponse(fd, status.success ? 200 : 500, status.success ? "OK" : "Internal Server Error", "application/json", makeJsonResponse(camera, status));
    } else if(request.method == "POST" && request.path == "/camera/record/stop") {
        const auto status = camera.stopRecording();
        std::clog << "停止录像请求" << (status.success ? "完成" : "失败") << (status.success ? "" : ": "+status.message) << std::endl;
        sendTextResponse(fd, 200, "OK", "application/json", makeJsonResponse(camera, status));
    } else if(request.method == "GET" && request.path == "/camera/stream.mjpeg") {
        handleMjpegStream(fd, camera, config);
    } else {
        sendTextResponse(fd, 404, "Not Found", "application/json", "{\"success\":false,\"message\":\"not found\",\"path\":\"\",\"cameraConnected\":false,\"recording\":false}");
    }
    } catch(const std::invalid_argument& e) {
        Json::Value v; v["success"] = false; v["message"] = e.what();
        sendTextResponse(fd,400,"Bad Request","application/json",oakJsonText(v));
    } catch(const std::logic_error& e) {
        Json::Value v; v["success"] = false; v["message"] = e.what();
        sendTextResponse(fd,409,"Conflict","application/json",oakJsonText(v));
    } catch(const std::exception& e) {
        Json::Value v; v["success"] = false; v["message"] = e.what();
        sendTextResponse(fd,500,"Internal Server Error","application/json",oakJsonText(v));
    }
}

OakCameraConfig parseConfig(int argc, char** argv) {
    OakCameraConfig config;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& name) -> std::string {
            if(i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + name);
            }
            return argv[++i];
        };
        auto integer = [&](const std::string& name) {
            const auto value = requireValue(name);
            std::size_t end = 0;
            const int result = std::stoi(value, &end);
            if(end != value.size()) throw std::invalid_argument("invalid integer for " + name);
            return result;
        };
        auto real = [&](const std::string& name) {
            const auto value = requireValue(name);
            std::size_t end = 0;
            const float result = std::stof(value, &end);
            if(end != value.size()) throw std::invalid_argument("invalid number for " + name);
            return result;
        };

        if(arg == "--bind") {
            config.bindAddress = requireValue(arg);
        } else if(arg == "--port") {
            config.port = integer(arg);
        } else if(arg == "--snapshot-dir") {
            config.snapshotDir = requireValue(arg);
        } else if(arg == "--recording-dir") {
            config.recordingDir = requireValue(arg);
        } else if(arg == "--width") {
            config.previewWidth = integer(arg);
        } else if(arg == "--height") {
            config.previewHeight = integer(arg);
        } else if(arg == "--settings-file") {
            config.settingsFile = requireValue(arg);
        } else if(arg == "--sensor-fps") {
            config.media.sensorFps = real(arg);
        } else if(arg == "--record-width") {
            config.media.recording.width = integer(arg);
        } else if(arg == "--record-height") {
            config.media.recording.height = integer(arg);
        } else if(arg == "--record-fps") {
            config.media.recording.fps = real(arg);
        } else if(arg == "--photo-fps") {
            config.media.photo.fps = real(arg);
        } else if(arg == "--fps") {
            config.previewFps = real(arg);
        } else if(arg == "--jpeg-quality") {
            config.jpegQuality = integer(arg);
        } else if(arg == "--disable-preview-jpeg-optimization") {
            config.optimizePreviewJpeg = false;
        } else if(arg == "--capture-jpeg-quality") {
            config.captureJpegQuality = integer(arg);
        } else if(arg == "--recording-crf") {
            config.recordingCrf = integer(arg);
        } else if(arg == "--frame-freshness-ms") {
            config.frameFreshnessMs = integer(arg);
        } else if(arg == "--no-frame-timeout-ms") {
            config.noFrameTimeoutMs = integer(arg);
        } else if(arg == "--reconnect-interval-ms") {
            config.reconnectIntervalMs = integer(arg);
        } else if(arg == "--reconnect-max-interval-ms") {
            config.reconnectMaxIntervalMs = integer(arg);
        } else if(arg == "--disable-low-latency-profile") {
            config.lowLatencyProfile = false;
        } else if(arg == "--disable-depth") {
            config.depthEnabled = false;
        } else if(arg == "--depth-fps") {
            config.depthFps = integer(arg);
        } else if(arg == "--depth-jpeg-quality") {
            config.depthJpegQuality = integer(arg);
        } else if(arg == "--depth-visual-min-mm") {
            config.depthVisualMinMm = integer(arg);
        } else if(arg == "--depth-visual-max-mm") {
            config.depthVisualMaxMm = integer(arg);
        } else if(arg == "--help" || arg == "-h") {
            std::cout << "Preview lossless entropy optimization: enabled; use --disable-preview-jpeg-optimization to disable.\n";
            std::cout << "Media settings: [--settings-file FILE] [--sensor-fps FPS] [--record-width W] [--record-height H] [--record-fps FPS] [--photo-fps FPS]. Runtime: GET /camera/capabilities, GET/PUT /camera/settings.\n";
            std::cout << "Stereo depth: [--disable-depth] [--depth-fps 1..15] [--depth-jpeg-quality 1..90] [--depth-visual-min-mm 300] [--depth-visual-max-mm 10000]. Runtime: GET /camera/depth/status, /image.png (uint16 mm), /image.jpg, /stream.mjpeg.\n";
            std::cout << "Usage: oak_camera_service [--bind ADDR] [--port PORT] [--snapshot-dir DIR] [--recording-dir DIR] [--width W] [--height H] [--fps FPS] [--jpeg-quality N] [--capture-jpeg-quality N] [--recording-crf N] [--disable-low-latency-profile] [--frame-freshness-ms N] [--no-frame-timeout-ms N] [--reconnect-interval-ms N] [--reconnect-max-interval-ms N]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    validateOakCameraConfig(config);
    if(config.lowLatencyProfile) {
        config.previewWidth = std::min(config.previewWidth, 960);
        config.previewHeight = std::min(config.previewHeight, 540);
        config.previewFps = std::min(config.previewFps, 15.0f);
        config.jpegQuality = std::min(config.jpegQuality, 70);
    }

    return config;
}

int makeServerSocket(const OakCameraConfig& config) {
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(serverFd < 0) {
        throw std::runtime_error("socket() failed");
    }

    int opt = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    ::setsockopt(serverFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(config.port));
    if(::inet_pton(AF_INET, config.bindAddress.c_str(), &address.sin_addr) != 1) {
        ::close(serverFd);
        throw std::runtime_error("invalid bind address: " + config.bindAddress);
    }

    if(::bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const auto err = std::string(std::strerror(errno));
        ::close(serverFd);
        throw std::runtime_error("bind() failed: " + err);
    }

    if(::listen(serverFd, 16) != 0) {
        const auto err = std::string(std::strerror(errno));
        ::close(serverFd);
        throw std::runtime_error("listen() failed: " + err);
    }

    return serverFd;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto config = parseConfig(argc, argv);
        cv::setNumThreads(1); // Keep media processing from claiming navigation CPU cores.
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        std::signal(SIGPIPE, SIG_IGN);

        // Acquire the port before opening the camera, so duplicates do not steal it.
        const int serverFd = makeServerSocket(config);
        OakCameraDevice camera(config);
        try { camera.start(); }
        catch(...) { ::close(serverFd); throw; }
        std::vector<std::unique_ptr<ClientConnection>> clients;
        std::cout << "相机服务已启动，监听 " << config.bindAddress << ':' << config.port << std::endl;

        while(gRunning.load()) {
            for(auto it = clients.begin(); it != clients.end();) {
                if((*it)->done.load()) {
                    (*it)->worker.join();
                    it = clients.erase(it);
                } else ++it;
            }
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            const int clientFd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if(clientFd < 0) {
                if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                std::cerr << "相机服务接受连接失败: " << std::strerror(errno) << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            timeval clientTimeout{};
            clientTimeout.tv_sec = 2;
            ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &clientTimeout, sizeof(clientTimeout));
            ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &clientTimeout, sizeof(clientTimeout));
            if(clients.size() >= 16) {
                sendTextResponse(clientFd, 503, "Service Unavailable", "application/json",
                                 "{\"success\":false,\"message\":\"too many camera clients\"}");
                ::close(clientFd);
                continue;
            }
            auto connection = std::make_unique<ClientConnection>();
            connection->fd = clientFd;
            auto* current = connection.get();
            clients.push_back(std::move(connection));
            try {
                current->worker = std::thread([current, &camera, &config] {
                    try { handleClient(current->fd, camera, config); }
                    catch(const std::exception& e) {
                        std::cerr << "相机 HTTP 客户端异常: " << e.what() << std::endl;
                    }
                    catch(...) { std::cerr << "相机 HTTP 客户端未知异常" << std::endl; }
                    {
                        std::lock_guard<std::mutex> lock(current->mutex);
                        ::shutdown(current->fd, SHUT_RDWR);
                        ::close(current->fd);
                        current->fd = -1;
                    }
                    current->done.store(true);
                });
            } catch(...) {
                ::close(clientFd);
                clients.pop_back();
                gRunning.store(false);
                std::cerr << "无法创建相机 HTTP 工作线程" << std::endl;
            }
        }

        ::close(serverFd);
        for(const auto& client : clients) {
            std::lock_guard<std::mutex> lock(client->mutex);
            if(client->fd >= 0) ::shutdown(client->fd, SHUT_RDWR);
        }
        camera.stop();
        for(const auto& client : clients) client->worker.join();
        std::cout << "相机服务已停止" << std::endl;
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "oak_camera_service error: " << e.what() << std::endl;
        return 1;
    }
}
