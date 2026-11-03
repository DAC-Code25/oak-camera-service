"""Verify depth transport, exact millimeters, freshness and bounded media admission.

Uses the production server/device with the test-only capture source; never USB.
PNG decoding below uses stdlib only, avoiding a Python OpenCV dependency.
"""
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def wait_for(predicate, reason, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(.02)
    raise AssertionError(reason)


def decode_png(png):
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    offset, payload = 8, b""
    while offset < len(png):
        size = struct.unpack(">I", png[offset:offset+4])[0]
        kind, data = png[offset+4:offset+8], png[offset+8:offset+8+size]
        if kind == b"IHDR":
            width, height, bits, color, _, _, interlace = struct.unpack(">IIBBBBB", data)
            assert bits == 16 and color == 0 and interlace == 0
        if kind == b"IDAT":
            payload += data
        offset += 12 + size
    packed = zlib.decompress(payload)
    stride = width * 2
    previous, pixels, cursor = bytearray(stride), [], 0
    for _ in range(height):
        method = packed[cursor]
        row = bytearray(packed[cursor+1:cursor+1+stride])
        cursor += stride + 1
        for x in range(stride):
            a, b, c = (row[x-2] if x >= 2 else 0), previous[x], (previous[x-2] if x >= 2 else 0)
            p = a + b - c
            distances = [abs(p-a), abs(p-b), abs(p-c)]
            paeth = [a, b, c][distances.index(min(distances))]
            predictor = [0, a, b, (a+b)//2, paeth][method]
            row[x] = (row[x] + predictor) & 255
        pixels.extend(struct.unpack(">"+"H"*width, row))
        previous = row
    return width, height, pixels


with tempfile.TemporaryDirectory(prefix="oak-depth-test-") as directory:
    state = Path(directory) / "state"
    state.write_text("healthy")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    env = dict(os.environ, OAK_TEST_STATE_FILE=str(state))
    args = [sys.argv[1], "--bind", "127.0.0.1", "--port", str(port),
            "--width", "48", "--height", "32", "--record-width", "96", "--record-height", "64",
            "--snapshot-dir", directory+"/photos", "--recording-dir", directory+"/records",
            "--frame-freshness-ms", "100", "--no-frame-timeout-ms", "400",
            "--reconnect-interval-ms", "30", "--reconnect-max-interval-ms", "100"]
    log = open(Path(directory)/"service.log", "w+")
    process = None
    clients = []
    responses = []

    def request(path):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        connection.request("GET", path)
        response = connection.getresponse()
        code, headers, body = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return code, headers, body

    def status():
        return json.loads(request("/camera/status")[2])

    try:
        process = subprocess.Popen(args, env=env, stdout=log, stderr=log)
        wait_for(lambda: status()["cameraConnected"], "camera did not start")
        info = status()["depth"]
        assert info["supported"] and not info["available"] and not info["transportActive"] and info["frameId"] == 0
        code, headers, png = request("/camera/depth/image.png")
        assert code == 200 and headers["Content-Type"] == "image/png"
        width, height, pixels = decode_png(png)
        assert (width,height) == (32,24) and pixels[0] == 0 and pixels[1] == 1234 and pixels[-1] == 1234
        assert headers["X-Depth-Unit"] == "millimeter" and headers["X-Depth-Reference"] == "rectified_left_camera"
        assert int(headers["X-Frame-Id"]) > 0 and int(headers["X-Captured-Monotonic-Ms"]) > 0
        code, headers, jpeg = request("/camera/depth/image.jpg")
        assert code == 200 and jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")
        connection = http.client.HTTPConnection("127.0.0.1",port,timeout=3)
        clients.append(connection)
        connection.request("GET","/camera/depth/stream.mjpeg")
        stream = connection.getresponse()
        assert stream.status == 200
        ids = []
        for _ in range(3):
            assert stream.readline().startswith(b"--tenco-oak-boundary")
            part_headers = {}
            while True:
                line = stream.readline().strip()
                if not line:
                    break
                key,value = line.decode().split(":",1)
                part_headers[key] = value.strip()
            image = stream.read(int(part_headers["Content-Length"]))
            assert image.startswith(b"\xff\xd8") and stream.read(2) == b"\r\n"
            ids.append(int(part_headers["X-Frame-Id"]))
        assert ids == sorted(set(ids)), "depth stream replayed old frame"
        # A failed depth stream does not stop RGB or rebuild the camera.
        retries = status()["reconnectCount"]
        state.write_text("depth-stall")
        assert len(stream.read()) < 1024*1024
        connection.close(); clients.clear()
        code, _, body = request("/camera/depth/image.png")
        assert code == 503 and not json.loads(body)["available"]
        assert status()["cameraConnected"] and status()["reconnectCount"] == retries
        # Stale source timestamps must never be exposed as current data.
        state.write_text("depth-old")
        assert request("/camera/depth/image.png")[0] == 503
        state.write_text("healthy")
        assert request("/camera/depth/image.png")[0] == 200
        time.sleep(2.3)
        idle = status()["depth"]
        assert not idle["transportActive"] and not idle["available"]
        frame_id = idle["frameId"]
        time.sleep(.25)
        assert status()["depth"]["frameId"] == frame_id, "idle depth still transferred"
        # Media slots are bounded separately; status remains available.
        for _ in range(4):
            c = http.client.HTTPConnection("127.0.0.1",port,timeout=3)
            clients.append(c); c.request("GET","/camera/stream.mjpeg")
            response = c.getresponse(); responses.append(response)
            assert response.status == 200
        assert request("/camera/depth/image.jpg")[0] == 503
        assert status()["cameraConnected"]
        for response in responses:
            response.close()
        responses.clear()
        for client in clients:
            client.close()
        clients.clear()
        process.terminate(); assert process.wait(timeout=4) == 0
        # Both unsupported hardware and explicit disable report 503, never fake depth.
        for mode,extra in [("no-depth",[]),("healthy",["--disable-depth"])]:
            state.write_text(mode)
            process = subprocess.Popen(args+extra,env=env,stdout=log,stderr=log)
            wait_for(lambda: status()["cameraConnected"], "camera variant did not start")
            assert request("/camera/depth/image.png")[0] == 503
            assert not status()["depth"]["available"]
            process.terminate(); assert process.wait(timeout=4) == 0
        invalid = subprocess.run([sys.argv[1],"--depth-fps","0"],capture_output=True,timeout=3)
        assert invalid.returncode != 0
        print("depth uint16 millimeters/JPEG/multiframe/idle/stale/EOF/admission/unsupported/disabled passed")
    finally:
        for response in responses:
            response.close()
        for client in clients:
            client.close()
        if process and process.poll() is None:
            process.kill(); process.wait()
        log.close()
