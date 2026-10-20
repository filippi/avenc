cmake_minimum_required(VERSION 3.18)
project(avenc_ext LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# PEP 517 installs pybind11 from pyproject.toml; do not clone dependencies.
set(PYBIND11_FINDPYTHON ON)
find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)
pybind11_add_module(_avenc_ext src/avenc_codec.cpp src/avenc_binding.cpp)
target_include_directories(_avenc_ext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(_avenc_ext PRIVATE /bigobj)
else()
  target_compile_options(_avenc_ext PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS _avenc_ext LIBRARY DESTINATION avenc_xarray
        ARCHIVE DESTINATION avenc_xarray RUNTIME DESTINATION avenc_xarray)
