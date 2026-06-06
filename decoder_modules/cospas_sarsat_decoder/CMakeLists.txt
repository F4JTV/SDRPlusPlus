cmake_minimum_required(VERSION 3.13)
project(cospas_sarsat_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(cospas_sarsat_decoder PRIVATE "src/")
