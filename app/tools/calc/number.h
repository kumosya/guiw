#pragma once
#include <cstdint>
#include <string>
namespace calc {
struct Result {
    std::string text;
    bool approximate = false;
    const char* error = nullptr;
};
Result evaluate(const char* expression, uint32_t precision = 100,
                const uint32_t* generation = nullptr, uint32_t expected = 0);
}  // namespace calc
