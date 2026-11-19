#pragma once

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

// Select real sensor frames before scaling or USB transport. The sensor sequence
// owns the cadence; Script scheduling and host/network activity do not.
inline std::string oakRgbGateScript(double outputFps, double sensorFps, int demandIndex) {
    if(!std::isfinite(outputFps) || outputFps <= 0 || !std::isfinite(sensorFps) || sensorFps <= 0 ||
       demandIndex < -1 || demandIndex > 2)
        throw std::invalid_argument("invalid RGB gate cadence");
    std::ostringstream script;
    script << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "enabled = " << (demandIndex < 0 ? "True" : "False") << "\n"
           << "next_sequence = None\nlast_sequence = None\n"
           << "while True:\n    frame = node.io['in'].get()\n";
    if(demandIndex >= 0) {
        script << "    control = node.io['control'].tryGet()\n"
               << "    if control is not None:\n"
               << "        enabled = control.getData()[" << demandIndex << "] == 1\n";
    }
    script << "    if not enabled:\n"
           << "        next_sequence = None\n        last_sequence = None\n        continue\n";
    if(outputFps >= sensorFps) {
        // Never rate-limit a stream already limited by the sensor itself.
        script << "    node.io['out'].send(frame)\n";
    } else {
        script << "    sequence = frame.getSequenceNum()\n"
               << "    if next_sequence is None or sequence <= last_sequence:\n"
               << "        next_sequence = float(sequence)\n"
               << "    last_sequence = sequence\n"
               << "    if sequence + 1e-9 >= next_sequence:\n"
               << "        node.io['out'].send(frame)\n"
               << "        next_sequence += " << sensorFps / outputFps << "\n"
               << "        if next_sequence <= sequence:\n"
               << "            next_sequence = sequence + " << sensorFps / outputFps << "\n";
    }
    return script.str();
}
