"""Execute the production-generated OAK Script with source/control queue mocks."""
import math
import subprocess
import sys
from types import SimpleNamespace
from unittest.mock import patch


class EndOfFrames(Exception):
    pass


def generated_script(fps, sensor_fps=30, demand=2):
    return subprocess.check_output(
        [sys.argv[1], str(fps), str(sensor_fps), str(demand)], text=True
    )


def selected(script, sequences, controls=None, execution_times=None):
    outputs = []
    position = -1

    def get_frame():
        nonlocal position
        position += 1
        if position == len(sequences):
            raise EndOfFrames
        return SimpleNamespace(getSequenceNum=lambda: sequences[position])

    def control():
        value = controls.get(position) if controls else (1, 1, 1)
        return None if value is None else SimpleNamespace(getData=lambda: value)

    node = SimpleNamespace(io={
        "in": SimpleNamespace(get=get_frame),
        "control": SimpleNamespace(tryGet=control),
        "out": SimpleNamespace(send=lambda frame: outputs.append(frame.getSequenceNum())),
    })
    # Device scheduling stalls may vary independently of the frames' sequence.
    times = execution_times or [1000 + index / 30 for index in range(len(sequences))]
    with patch("time.monotonic", side_effect=times):
        try:
            exec(script, {"node": node})
        except EndOfFrames:
            pass
    return outputs


def main():
    frames = list(range(3600))
    regular = [1000 + i / 30 for i in frames]
    jittered = [1000 + i / 30 + 0.2 * (i // 11) for i in frames]
    for fps, sensor in [(30, 30), (5, 5)]:
        script = generated_script(fps, sensor)
        assert selected(script, frames, execution_times=jittered) == frames, "sensor-rate frames lost"
    for fps in (25, 15, 12.5, 7.25, 29.97):
        script = generated_script(fps)
        output = selected(script, frames, execution_times=regular)
        assert abs(len(output) - len(frames) * fps / 30) <= 1, (fps, len(output))
        assert selected(script, frames, execution_times=jittered) == output, "execution jitter changed cadence"
        assert len(set(output)) == len(output), "frame duplicated"

    # Every new demand starts immediately, irrespective of the previous phase.
    controls = {0: (0, 0, 0), 5: (0, 0, 1), 10: (0, 0, 0), 20: (0, 0, 1)}
    assert selected(generated_script(10), list(range(30)), controls) == [5, 8, 20, 23, 26, 29]
    # Missing source frames must not cause replay or a burst to catch up.
    output = selected(generated_script(10), [0, 1, 2] + list(range(100, 120)))
    assert output == [0, 100, 103, 106, 109, 112, 115, 118], output
    # A device sequence reset establishes a new phase immediately.
    assert selected(generated_script(10), [0, 1, 2, 0, 1, 2, 3]) == [0, 0, 3]
    assert selected(generated_script(2, demand=-1), list(range(60))) == [0, 15, 30, 45]
    for value in (0, -1, math.nan, math.inf):
        result = subprocess.run([sys.argv[1], str(value), "30", "2"], capture_output=True)
        assert result.returncode != 0, "invalid cadence accepted"
    print("RGB sensor cadence, fractional FPS, scheduling jitter, demand restart and source gaps passed")


if __name__ == "__main__":
    main()
