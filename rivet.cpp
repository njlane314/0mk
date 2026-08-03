#include "rivet.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        rivet::engine engine;

        // Demonstration profile. Replace this with a Burst, SSH, container, or
        // scheduler-backed executor without changing rivet.h or the Rivetfile.
        engine.profile("large", rivet::local_executor("large-local-demo-v1"));

        return engine.cli(argc, argv);
    } catch (const std::exception& failure) {
        std::cerr << "rivet: " << failure.what() << '\n';
        return 2;
    }
}
