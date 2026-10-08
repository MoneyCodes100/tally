#include "tally/hyperloglog.hpp"

#include <fstream>
#include <iostream>
#include <string>

// One-file C++ driver. The same sketch is what the Python `tally card` command calls.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: tally_example <file>\n";
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "could not open " << argv[1] << "\n";
        return 1;
    }

    try {
        tally::HyperLogLog sketch(14);
        std::string line;
        std::uint64_t rows = 0;
        while (std::getline(input, line)) {
            sketch.add(line);
            ++rows;
        }
        std::cout << "lines " << rows << "\n";
        std::cout << "estimate " << sketch.estimate() << "\n";
        std::cout << "bytes " << sketch.size_bytes() << "\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
