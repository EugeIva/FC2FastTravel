cmake_minimum_required(VERSION 3.20)
project(fc2_fast_travel CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Far Cry 2 is 32-bit: build with the x86 toolchain (vcvars32).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The mod is built as a proxy dinput8.dll: Dunia.dll imports DirectInput8Create
# from it, so Windows loads it from the game's bin folder with no extra loader.
add_library(dinput8 SHARED
  src/dllmain.cpp
  src/log.cpp
  src/game.cpp src/input.cpp src/overlay.cpp src/mapscreen.cpp src/archive.cpp
  src/dinput8.def)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(dinput8 PRIVATE /W4)
