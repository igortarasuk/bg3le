#pragma once

#include <string>
#include <vector>

#include "osi.h"

namespace bg3le {

// Brings up the embedded Lua state. Safe to call more than once.
void lua_init();

// Publishes the enumerated Osiris functions as the global Osi table.
void lua_bind_osi(const std::vector<osi::Function>& functions);

// Runs a chunk, logging the result or the error. For bring-up checks.
void lua_run(const char* code);

// Evaluates a chunk, returning its stringified results or the error text.
// Must be called from the story thread.
void lua_eval(const char* code, std::string* result, std::string* error);

}  // namespace bg3le
