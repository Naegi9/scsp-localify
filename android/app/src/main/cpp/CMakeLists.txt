cmake_minimum_required(VERSION 3.22.1)
project(scsplocal CXX)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

find_package(shadowhook REQUIRED CONFIG)

add_library(scsplocal SHARED main.cpp)
target_link_libraries(scsplocal shadowhook::shadowhook nlohmann_json::nlohmann_json log)
