cmake_minimum_required(VERSION 3.21)
project(rkvc-cli VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT TARGET rkvc::core)
    add_subdirectory(../core core-build)
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

add_executable(rkvc-cli src/main.cpp)
set_target_properties(rkvc-cli PROPERTIES
    OUTPUT_NAME rkvc
    ENABLE_EXPORTS ON)
target_link_libraries(rkvc-cli PRIVATE rkvc::core)
target_compile_options(rkvc-cli PRIVATE -fno-exceptions -fno-rtti
    -Wall -Wextra)
