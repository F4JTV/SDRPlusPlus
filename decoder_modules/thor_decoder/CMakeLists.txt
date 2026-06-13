cmake_minimum_required(VERSION 3.13)
project(thor_decoder)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(thor_decoder PRIVATE "src/")
