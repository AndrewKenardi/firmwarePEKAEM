# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Add flag to treat missing-field-initializers as a warning instead of an error
add_compile_options(-Wno-error=missing-field-initializers)
add_compile_options(-Wno-error=cpp)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(FirmwarePkm VERSION "1.0.44")
