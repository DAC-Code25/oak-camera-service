"""Encoded capture -> production HTTP/device -> muxer integration; no hardware or FPS claim."""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, reason, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(.02)
    raise AssertionError(reason)


def generate_payload(path, encoding):
    command = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
               "color=c=orange:s=96x64:r=30", "-frames:v", "1", "-threads", "1"]
    if encoding == "h264":
        command += ["-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                    "-x264-params", "repeat-headers=1:keyint=1", "-pix_fmt", "yuv420p", "-f", "h264"]
    else:
        command += ["-c:v", "mjpeg", "-q:v", "2", "-pix_fmt", "yuvj420p", "-f", "mjpeg"]
    subprocess.run(command + [str(path)], check=True, capture_output=True, timeout=10)


def exercise(fixture, encoding, directory):
    root = Path(directory)
    payload = root / ("payload." + encoding)
    generate_payload(payload, encoding)
    state = root / "state"
    state.write_text("healthy")
    env = dict(os.environ, OAK_TEST_STATE_FILE=str(state), OAK_TEST_ENCODED_FILE=str(payload),
               OAK_TEST_RECORDING_ENCODING=encoding)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    command = [fixture, "--bind", "127.0.0.1", "--port", str(port),
               "--width", "48", "--height", "32", "--record-width", "96", "--record-height", "64",
               "--snapshot-dir", directory, "--recording-dir", directory]
    with open(root / "service.log", "w+") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log)

        def request(path="/camera/status", method="GET"):
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
            try:
                connection.request(method, path)
                response = connection.getresponse()
                body = response.read()
                assert response.status == 200, body
                return json.loads(body)
            finally:
                connection.close()

        try:
            wait_for(lambda: request()["cameraConnected"], "encoded camera did not become ready")
            caps = request("/camera/capabilities")
            expected_container, expected_suffix = ("mp4", ".mp4") if encoding == "h264" else ("matroska", ".mkv")
            assert (caps["recordingCodec"], caps["recordingContainer"], caps["recordingEncoder"]) == (
                encoding, expected_container, "oak_hardware"), caps
            paths = []
            for _ in range(2):
                # A new writer must establish codec headers again after idle.
                started = request("/camera/record/start", "POST")
                assert started["success"] and started["recording"], started
                assert started["recordingFrames"] >= 1, "start acknowledged before a decodable frame committed"
                assert started["recordingEncoder"] == "oak_hardware", started
                stopped = request("/camera/record/stop", "POST")
                assert stopped["success"] and not stopped["recording"] and stopped["recordingFrames"] >= 1, stopped
                assert not stopped["lastRecordingError"], stopped
                assert Path(stopped["path"]).suffix == expected_suffix, stopped
                assert stopped["path"] not in paths, "second recording overwrote the first"
                paths.append(stopped["path"])
                subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", stopped["path"], "-f", "null", "-"],
                               check=True, capture_output=True, timeout=10)
                probe = json.loads(subprocess.check_output(
                    ["ffprobe", "-v", "error", "-count_frames", "-show_entries",
                     "stream=codec_name,width,height,nb_read_frames", "-of", "json", stopped["path"]], timeout=10
                ))["streams"][0]
                assert (probe["codec_name"], probe["width"], probe["height"]) == (encoding, 96, 64), probe
                assert int(probe["nb_read_frames"]) == stopped["recordingFrames"], (probe, stopped)
                time.sleep(.15)
                idle = request()
                assert idle["cameraConnected"] and not idle["recording"], idle
                assert not idle["mediaConfiguration"]["recordingTransportActive"], idle
                assert idle["recordingFrames"] == stopped["recordingFrames"], "recording resumed after stop"
            started = request("/camera/record/start", "POST")
            assert started["success"], started
            state.write_text("recording-invalid")
            failed = wait_for(lambda: (s if (s := request()).get("lastRecordingError") and
                not s["recording"] and not s["mediaConfiguration"]["recordingTransportActive"] else None),
                "failed writer kept recording transport enabled")
            assert failed["cameraConnected"], failed
            state.write_text("healthy")
            time.sleep(.1)
            assert not request()["recording"], "failed recording restarted automatically"
            restarted = request("/camera/record/start", "POST")
            assert restarted["success"] and restarted["recording"], restarted
            assert request("/camera/record/stop", "POST")["success"]
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            log.flush()
            log.seek(0)
            print(log.read())


with tempfile.TemporaryDirectory(prefix="oak-encoded-http-") as directory:
    for codec in ("h264", "mjpeg"):
        target = Path(directory) / codec
        target.mkdir()
        exercise(sys.argv[1], codec, str(target))
print("H264/MJPEG encoded HTTP startup commitment, metadata, decode, stop and second recording passed")
