#include "oak_rgb_gate_script.h"
#include <iostream>

int main(int argc, char** argv) {
    if(argc != 4) return 2;
    try {
        std::cout << oakRgbGateScript(std::stod(argv[1]), std::stod(argv[2]), std::stoi(argv[3]));
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
