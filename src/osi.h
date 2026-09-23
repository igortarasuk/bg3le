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

// The low 3 bits of a function id give its kind. 1-3 are the engine's own
// functions; the rest belong to the story -- a Proc_* a mod calls is kind
// 5, a user query kind 8.
enum Kind {
    kCall = 1,
    kQuery = 2,
    kEvent = 3,
    kDatabase = 4,
    kProc = 5,
    kSysQuery = 6,
    kSysCall = 7,
    kUserQuery = 8,
};

struct Function {
    std::string name;
    std::uint32_t id = 0;
    std::vector<std::uint8_t> params;

    // Which trailing parameters the engine fills in. Known only once the
    // signature database has been read; -1 until then, in which case the
    // caller's argument count decides the split.
    int out_params = -1;

    Kind kind() const { return static_cast<Kind>(id & 7); }

    // Which of the two dispatch handlers runs it.
    bool is_query() const {
        const Kind k = kind();
        return k == kQuery || k == kSysQuery || k == kUserQuery;
    }
};

// Reads out-parameter counts and parameter types from Osiris' own function
// database, keyed by name. Returns the number recovered, or 0 if it could
// not be read at all.
//
// `story` identifies the compiled story, and the answer is cached under
// it: the database is a property of the story, and walking it costs a
// hundred and thirty thousand reads on the story thread during level
// load. Pass nullptr to walk unconditionally.
// `cached` reports whether the answer came from the store rather than a
// walk, so the caller can say which.
std::size_t load_out_param_counts(std::vector<Function>* functions,
                                  char const* story, bool* cached);

// The functions the story itself defines -- procedures, user queries and
// databases -- which the engine's own mapping does not list. `known` is
// what that mapping gave, so the same function is not returned twice.
std::vector<Function> story_functions(std::vector<Function> const& known);

// How many the database held that story_functions could not return, which
// on this build is all of them: they carry no dispatch handle.
std::size_t story_function_count();

// How many nodes Osiris' node list holds, or 0 if it was not found.
std::size_t node_count();

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
