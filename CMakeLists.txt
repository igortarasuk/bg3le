cmake_minimum_required(VERSION 3.16)
project(bg3le CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(bg3le SHARED
    src/preload.cpp
    src/elf_symbols.cpp
    src/log.cpp
    src/mem.cpp
)
target_link_libraries(bg3le PRIVATE dl)
target_link_options(bg3le PRIVATE -static-libstdc++ -static-libgcc)
target_compile_options(bg3le PRIVATE -Wall -Wextra -fvisibility=default)

add_executable(symdump tools/symdump.cpp src/elf_symbols.cpp src/log.cpp)
