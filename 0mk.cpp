#include "0mk.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        mk0::engine engine;
        return engine.cli(argc, argv);
    } catch (const std::exception& failure) {
        std::cerr << "0mk: " << failure.what() << '\n';
        return 2;
    }
}
