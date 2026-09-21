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

    // Which trailing parameters the engine fills in. Known only once the
    // signature database has been read; -1 until then, in which case the
    // caller's argument count decides the split.
    int out_params = -1;

    Kind kind() const { return static_cast<Kind>(id & 7); }
};

// Reads out-parameter counts from Osiris' own function database, keyed by
// name. Returns the number recovered, or 0 if the walk failed.
std::size_t load_out_param_counts(std::vector<Function>* functions);

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

enum class Status {
    kHandled,      // the engine ran it and reported success
    kRejected,     // the engine ran it and reported false
    kUnavailable,  // could not be invoked at all
};

// Invokes fn with inputs, appending any out-parameters to outputs.
// Distinguishing kRejected from kUnavailable matters: a query returning
// false is a normal answer, not a failure.
Status invoke(const Function& fn, const std::vector<Value>& inputs,
              std::vector<Value>* outputs);

}  // namespace bg3le::osi
