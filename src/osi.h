// Invoking Osiris functions through the game's DIV dispatch handlers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bg3le::osi {

// TOsiValueType. 1-5 are built in; 6+ are the story's own enum types.
enum ValueType : unsigned short {
    kNone = 0,
    kInteger = 1,
    kInteger64 = 2,
    kReal = 3,
    kString = 4,
    kGuidString = 5,
};

// The low 3 bits of a function id give its kind.
enum Kind { kCall = 1, kQuery = 2, kEvent = 3 };

struct Function {
    std::string name;
    std::uint32_t id = 0;
    std::vector<std::uint8_t> params;

    Kind kind() const { return static_cast<Kind>(id & 7); }
};

// A value crossing the boundary in either direction.
struct Value {
    unsigned short type = kNone;
    std::int64_t integer = 0;
    double real = 0.0;
    std::string text;
};

// Records the dispatch handlers lifted from the callback table.
void set_handlers(void* call, void* query);
bool ready();

// Invokes fn with inputs, appending any out-parameters to outputs.
// Returns false if the engine rejected the call.
bool invoke(const Function& fn, const std::vector<Value>& inputs,
            std::vector<Value>* outputs);

}  // namespace bg3le::osi
