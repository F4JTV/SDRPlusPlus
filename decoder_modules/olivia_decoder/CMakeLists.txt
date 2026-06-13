cmake_minimum_required(VERSION 3.13)
project(olivia_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(olivia_decoder PRIVATE "src/")
