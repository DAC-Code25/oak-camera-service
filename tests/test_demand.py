"""Real HTTP worker/device paths with demand-aware test capture (no USB SDK)."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, reason, timeout=5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(.02)
    raise AssertionError(reason)


with tempfile.TemporaryDirectory(prefix="oak-demand-test-") as directory:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    args = [sys.argv[1], "--bind", "127.0.0.1", "--port", str(port),
            "--width", "48", "--height", "32", "--record-width", "96", "--record-height", "64",
            "--snapshot-dir", directory, "--recording-dir", directory]
    log = open(Path(directory) / "log", "w+")
    process = subprocess.Popen(args, stdout=log, stderr=log)

    def request(path="/camera/status", method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        connection.request(method, path)
        response = connection.getresponse()
        assert response.status == 200, response.read()
        result = json.loads(response.read())
        connection.close()
        return result

    try:
        wait_for(lambda: request()["cameraConnected"], "camera did not become ready")
        time.sleep(.15)  # Drain the bounded startup validation frames.
        idle = request()
        time.sleep(1.1)
        later = request()
        assert later["cameraConnected"] and later["captureFrameId"] > idle["captureFrameId"]
        assert later["lastCaptureFrameAgeMs"] <= 750
        assert later["frameId"] == idle["frameId"], "idle preview still encoded/transferred"
        assert later["nativeFrameId"] == idle["nativeFrameId"], "idle native photos still transferred"
        media = later["mediaConfiguration"]
        assert not any(media[key] for key in ("previewTransportActive", "nativeTransportActive", "recordingTransportActive"))

        # A new subscriber wakes an idle preview, not a cached startup JPEG.
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        connection.request("GET", "/camera/stream.mjpeg")
        response = connection.getresponse()
        assert response.status == 200
        assert response.readline().startswith(b"--tenco-oak-boundary")
        assert response.readline().strip() == b"Content-Type: image/jpeg"
        length = int(response.readline().split(b":")[1])
        assert response.readline() == b"\r\n"
        assert response.read(length).startswith(b"\xff\xd8")
        response.close(); connection.close()
        assert request()["frameId"] > idle["frameId"]
        time.sleep(2.3)  # Transport lease expires even if disconnect is slow.
        idle = request()
        time.sleep(.25)
        assert request()["frameId"] == idle["frameId"]

        # Every photo must wait for a frame captured after its own request.
        native_id = request()["nativeFrameId"]
        first = request("/camera/photo", "POST")
        assert first["success"] and first["nativeFrameId"] > native_id
        second = request("/camera/photo", "POST")
        assert second["success"] and second["nativeFrameId"] > first["nativeFrameId"]
        assert first["path"] != second["path"]
        time.sleep(.1)
        assert not request()["mediaConfiguration"]["nativeTransportActive"]

        # Stopping immediately after start still retains/encodes its first frame.
        started = request("/camera/record/start", "POST")
        assert started["success"] and started["recording"]
        stopped = request("/camera/record/stop", "POST")
        assert stopped["success"] and stopped["recordingFrames"] >= 1
        subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", stopped["path"], "-f", "null", "-"],
                       check=True, capture_output=True)
        assert not request()["mediaConfiguration"]["recordingTransportActive"]
        print("idle health/no unused RGB/native/record transport, preview wake, new photo frame and first recording frame passed")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        log.flush(); log.seek(0); print(log.read()); log.close()
