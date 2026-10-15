"""Settings HTTP contract, optimistic concurrency, rollback and persistence."""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, timeout=5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(.02)
    raise AssertionError("settings condition timed out")


with tempfile.TemporaryDirectory(prefix="oak-settings-test-") as directory:
    root = Path(directory)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    args = [sys.argv[1], "--bind", "127.0.0.1", "--port", str(port),
            "--width", "48", "--height", "32", "--record-width", "96", "--record-height", "64",
            "--settings-file", str(root / "settings.json"), "--snapshot-dir", directory,
            "--recording-dir", directory, "--frame-freshness-ms", "100", "--no-frame-timeout-ms", "300",
            "--reconnect-interval-ms", "20"]
    log = open(root / "log", "w+")
    process = subprocess.Popen(args, stdout=log, stderr=log)

    def request(path="/camera/settings", method="GET", body=None):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
        connection.request(method, path, None if body is None else json.dumps(body),
                           {"Content-Type": "application/json"})
        response = connection.getresponse()
        result = response.status, json.loads(response.read())
        connection.close()
        return result

    def ready():
        v = request()[1]
        return v if v.get("cameraConnected") and v.get("applyState") != "pending" else None

    def body(settings, revision):
        return dict(schemaVersion=1, expectedRevision=revision, settings=settings)

    try:
        current = wait_for(ready)
        caps = request("/camera/capabilities")[1]
        assert caps["native"]["width"] == 96 and caps["native"]["maxFps"] == 30
        initial = current["settings"]
        invalid = json.loads(json.dumps(initial))
        invalid["recording"]["width"] = 95
        assert request(method="PUT", body=body(invalid, current["revision"]))[0] == 400
        assert request(method="PUT", body=body(initial, "stale"))[0] == 409
        changed = json.loads(json.dumps(initial))
        changed["recording"] = dict(width=64, height=48, fps=20)
        changed["photo"] = dict(width=64, height=48, fps=2)
        update = body(changed, current["revision"])
        # Fragmented body must be read fully and the actual HTTP verb must be PUT.
        data = json.dumps(update).encode()
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            sock.sendall(b"PUT /camera/settings HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                         str(len(data)).encode() + b"\r\n\r\n" + data[:12])
            time.sleep(.03)
            sock.sendall(data[12:])
            response = http.client.HTTPResponse(sock)
            response.begin()
            assert response.status == 202, response.read()
            pending = json.loads(response.read())
        assert pending["revision"] != current["revision"] and pending["applyState"] == "pending"
        assert request(method="PUT", body=update)[0] == 409
        current = wait_for(ready)
        assert current["activeSettings"] == changed and current["settings"] == changed
        assert json.loads((root / "settings.json").read_text())["settings"] == changed
        photo = request("/camera/photo", "POST")[1]
        info = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-of", "json", photo["path"]]))["streams"][0]
        assert (info["width"], info["height"]) == (64, 48)
        assert request("/camera/record/start", "POST")[1]["success"]
        assert request(method="PUT", body=body(initial, current["revision"]))[0] == 409
        time.sleep(.25)
        stopped = request("/camera/record/stop", "POST")[1]
        info = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-of", "json", stopped["path"]]))["streams"][0]
        assert (info["width"], info["height"]) == (64, 48) and stopped["recordingActualFps"] > 0
        # A device-level failure must expose rejection and restore the good settings.
        rejected = json.loads(json.dumps(changed))
        rejected["recording"]["width"] = 80
        assert request(method="PUT", body=body(rejected, current["revision"]))[0] == 202
        current = wait_for(ready)
        assert current["applyState"] == "rejected" and "unsupported" in current["lastError"]
        assert current["settings"] == changed and current["activeSettings"] == changed
        assert request(method="PUT", body=body(changed, current["revision"]))[0] == 200
        process.terminate()
        assert process.wait(timeout=4) == 0
        process = subprocess.Popen(args, stdout=log, stderr=log)
        current = wait_for(ready)
        assert current["activeSettings"] == changed and current["applyState"] == "applied"
        assert not current["recording"], "restart must not resume a recording"
        process.terminate(); assert process.wait(timeout=4) == 0
        # An interrupted candidate application retains an on-disk good fallback.
        saved = json.loads((root / "settings.json").read_text())
        saved["fallbackSettings"] = changed
        saved["settings"] = rejected
        (root / "settings.json").write_text(json.dumps(saved))
        process = subprocess.Popen(args, stdout=log, stderr=log)
        current = wait_for(ready)
        assert current["applyState"] == "rejected" and current["activeSettings"] == changed
        assert "fallbackSettings" not in json.loads((root / "settings.json").read_text())
        print("capabilities/fragmented PUT/validation/conflict/apply/record guard/rollback/persistence passed")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        log.flush(); log.seek(0)
        print(log.read())
        log.close()
