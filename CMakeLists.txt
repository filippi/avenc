cmake_minimum_required(VERSION 3.18)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AVENC_VERSION LIMIT_COUNT 1)
project(AVENC VERSION ${AVENC_VERSION} LANGUAGES CXX)
option(AVENC_BUILD_PYTHON "Build Python bindings and wheel payload" OFF)
option(AVENC_PYTHON_ECCODES "Use pinned binary ecCodes from the PEP 517 environment" OFF)
if(AVENC_BUILD_PYTHON)
  find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
  set(AVENC_BINDIR "avenc/_native/bin")
  set(AVENC_SHAREDIR "avenc/_native/share/avenc")
else()
  set(AVENC_BINDIR "bin")
  set(AVENC_SHAREDIR "share/avenc")
endif()
add_compile_definitions(AVENC_VERSION="${PROJECT_VERSION}")
if(AVENC_PYTHON_ECCODES)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(DEFINED ENV{AVENC_ECCODES_PREFIX})
      set(AVENC_ECCODES_ROOT "$ENV{AVENC_ECCODES_PREFIX}")
    else()
      set(AVENC_ECCODES_ROOT "${CMAKE_BINARY_DIR}/eccodes-install")
      execute_process(COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/build_eccodes.py"
        --prefix "${AVENC_ECCODES_ROOT}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
  else()
    execute_process(COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/find_eccodes.py"
      OUTPUT_VARIABLE AVENC_ECCODES_ROOTS OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    list(GET AVENC_ECCODES_ROOTS 0 AVENC_ECCODES_ROOT)
    list(GET AVENC_ECCODES_ROOTS 1 AVENC_ECKIT_ROOT)
  endif()
  set(ECCODES_INCLUDE_DIR "${AVENC_ECCODES_ROOT}/include" CACHE PATH "" FORCE)
  find_library(ECCODES_LIB NAMES eccodes PATHS "${AVENC_ECCODES_ROOT}/lib" "${AVENC_ECCODES_ROOT}/lib64" NO_DEFAULT_PATH REQUIRED)
  set(CMAKE_BUILD_RPATH "${AVENC_ECCODES_ROOT}/lib;${AVENC_ECCODES_ROOT}/lib64")
  if(AVENC_ECKIT_ROOT)
    list(APPEND CMAKE_BUILD_RPATH "${AVENC_ECKIT_ROOT}/lib" "${AVENC_ECKIT_ROOT}/lib64")
  endif()
endif()

# ----------------------------------
# C++ standard
# ----------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------
# Build options
# ----------------------------------
option(ENABLE_ECCODES "Enable ECCodes support (required)" ON)
option(ENABLE_FAST_MATH "Enable -ffast-math (may alter NaN/Inf behavior)" OFF)
option(BUILD_AVENC_SUBSET_SHARED
       "Build avenc_subset_decode as a shared library (useful for ctypes/xarray integrations)"
       ON)

# ----------------------------------
# Build type defaults
# ----------------------------------
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ----------------------------------
# Compiler flags
# ----------------------------------
option(ENABLE_NATIVE_ARCH "Optimize for the build machine; binaries may not be portable" OFF)
option(ENABLE_IPO "Enable interprocedural optimization in Release when supported" ON)
include(CheckCXXCompilerFlag)
if(ENABLE_NATIVE_ARCH AND NOT MSVC)
  check_cxx_compiler_flag("-march=native" AVENC_HAS_MARCH_NATIVE)
  if(AVENC_HAS_MARCH_NATIVE)
    add_compile_options("$<$<CONFIG:Release>:-march=native>")
  endif()
endif()
if(ENABLE_FAST_MATH AND NOT MSVC)
  check_cxx_compiler_flag("-ffast-math" AVENC_HAS_FAST_MATH)
  if(AVENC_HAS_FAST_MATH)
    add_compile_options("$<$<CONFIG:Release>:-ffast-math>")
  endif()
endif()
if(ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
  if(IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(STATUS "IPO/LTO not enabled: ${IPO_ERROR}")
  endif()
endif()

# ----------------------------------
# Output directories
# ----------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin" CACHE PATH "AVENC build output directory")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin" CACHE PATH "AVENC build output directory")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../lib" CACHE PATH "AVENC build output directory")

# ----------------------------------
# Dependencies
# ----------------------------------
find_package(ZLIB REQUIRED)
# Installed tools find the subset library beside their executable.
if(APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

if(NOT ENABLE_ECCODES)
  message(FATAL_ERROR "ECCodes is required for AVENC tools; do not disable ENABLE_ECCODES.")
endif()

set(_ECCODES_HINTS "")
if(DEFINED ENV{ECCODES_HOME})
  set(ECCODES_HOME $ENV{ECCODES_HOME})
  list(APPEND _ECCODES_HINTS ${ECCODES_HOME})
  list(APPEND _ECCODES_HINTS ${ECCODES_HOME}/lib ${ECCODES_HOME}/lib64 ${ECCODES_HOME}/include)
endif()

find_path(ECCODES_INCLUDE_DIR
  NAMES eccodes.h
  HINTS ${_ECCODES_HINTS}
  PATH_SUFFIXES include
)

find_library(ECCODES_LIB
  NAMES eccodes
  HINTS ${_ECCODES_HINTS}
  PATH_SUFFIXES lib lib64
)

if(NOT ECCODES_INCLUDE_DIR OR NOT ECCODES_LIB)
  message(FATAL_ERROR
    "ECCodes not found. Set ECCODES_HOME or CMAKE_PREFIX_PATH to your eccodes install.")
endif()

message(STATUS "ECCodes include: ${ECCODES_INCLUDE_DIR}")
message(STATUS "ECCodes library: ${ECCODES_LIB}")

# Native source installs use an external ecCodes installation. Wheels bundle
# their dependencies beside the tools and retain only loader-relative paths.
if(NOT AVENC_BUILD_PYTHON)
  get_filename_component(_AVENC_ECCODES_LIBDIR "${ECCODES_LIB}" DIRECTORY)
  list(APPEND CMAKE_INSTALL_RPATH "${_AVENC_ECCODES_LIBDIR}")
endif()

# ----------------------------------
# Sources
# ----------------------------------
set(SRC_AVPACK src/avpack.cpp)
set(SRC_GRIB2AVENC src/grib2avenc.cpp)
set(SRC_AVENC2GRIB src/avenc2grib.cpp)
set(SRC_AVENC_SUBSET src/avenc_subset_decode.cpp)
set(SRC_GRIB_PAIR_EXTRACT src/grib_pair_extract.cpp)
set(SRC_AVENC_TEST_GENERATOR src/avenc_test_generator.cpp)
set(SRC_COMPUTE_ERROR src/ComputeError.cpp)

# ----------------------------------
# Core AVENC tools
# ----------------------------------

# Low-level packer used by grib2avenc
add_executable(avpack ${SRC_AVPACK})
target_include_directories(avpack PRIVATE ${ECCODES_INCLUDE_DIR})
if(TARGET ZLIB::ZLIB)
  target_link_libraries(avpack PRIVATE ${ECCODES_LIB} ZLIB::ZLIB)
else()
  target_link_libraries(avpack PRIVATE ${ECCODES_LIB} ${ZLIB_LIBRARIES})
  target_include_directories(avpack PRIVATE ${ZLIB_INCLUDE_DIRS})
endif()
install(TARGETS avpack RUNTIME DESTINATION ${AVENC_BINDIR})

# CLI wrapper profile selector
add_executable(grib2avenc ${SRC_GRIB2AVENC})
install(TARGETS grib2avenc RUNTIME DESTINATION ${AVENC_BINDIR})

# Subset decode C API (used by avenc2grib subset mode and external integrations)
if(BUILD_AVENC_SUBSET_SHARED)
  add_library(avenc_subset_decode SHARED ${SRC_AVENC_SUBSET})
else()
  add_library(avenc_subset_decode STATIC ${SRC_AVENC_SUBSET})
endif()
target_include_directories(avenc_subset_decode
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(TARGET ZLIB::ZLIB)
  target_link_libraries(avenc_subset_decode PRIVATE ZLIB::ZLIB)
else()
  target_link_libraries(avenc_subset_decode PRIVATE ${ZLIB_LIBRARIES})
  target_include_directories(avenc_subset_decode PRIVATE ${ZLIB_INCLUDE_DIRS})
endif()
install(TARGETS avenc_subset_decode
  LIBRARY DESTINATION ${AVENC_BINDIR}
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION ${AVENC_BINDIR}
)

# MP4 -> GRIB + subset float32 decoder
add_executable(avenc2grib ${SRC_AVENC2GRIB})
if(AVENC_BUILD_PYTHON)
  target_compile_definitions(avenc2grib PRIVATE AVENC_HTTP_HELPER_PATH="")
else()
  target_compile_definitions(avenc2grib PRIVATE
    AVENC_HTTP_HELPER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/python/avenc/avenc_xarray/source.py")
endif()
install(FILES python/avenc/avenc_xarray/source.py DESTINATION ${AVENC_SHAREDIR})
target_include_directories(avenc2grib PRIVATE ${ECCODES_INCLUDE_DIR})
target_link_libraries(avenc2grib PRIVATE avenc_subset_decode ${ECCODES_LIB})
if(TARGET ZLIB::ZLIB)
  target_link_libraries(avenc2grib PRIVATE ZLIB::ZLIB)
else()
  target_link_libraries(avenc2grib PRIVATE ${ZLIB_LIBRARIES})
  target_include_directories(avenc2grib PRIVATE ${ZLIB_INCLUDE_DIRS})
endif()
install(TARGETS avenc2grib RUNTIME DESTINATION ${AVENC_BINDIR})

# Paired GRIB extractor for error/stat pipelines
add_executable(grib_pair_extract ${SRC_GRIB_PAIR_EXTRACT})
target_include_directories(grib_pair_extract PRIVATE ${ECCODES_INCLUDE_DIR})
target_link_libraries(grib_pair_extract PRIVATE ${ECCODES_LIB})
install(TARGETS grib_pair_extract RUNTIME DESTINATION ${AVENC_BINDIR})

# Synthetic analytical test dataset generator (tests 1/2/4/6/7/8)
add_executable(avenc_test_generator ${SRC_AVENC_TEST_GENERATOR})
target_include_directories(avenc_test_generator PRIVATE ${ECCODES_INCLUDE_DIR})
target_link_libraries(avenc_test_generator PRIVATE ${ECCODES_LIB})
install(TARGETS avenc_test_generator RUNTIME DESTINATION ${AVENC_BINDIR})

# Raw-vs-GRIB error computation utility
add_executable(ComputeError ${SRC_COMPUTE_ERROR})
target_include_directories(ComputeError PRIVATE ${ECCODES_INCLUDE_DIR})
target_link_libraries(ComputeError PRIVATE ${ECCODES_LIB})
install(TARGETS ComputeError RUNTIME DESTINATION ${AVENC_BINDIR})

# Scientific identity-matched GRIB/AVENC comparison (legacy ComputeError retained).
add_executable(avenc-compare src/avenc_compare.cpp)
target_include_directories(avenc-compare PRIVATE ${ECCODES_INCLUDE_DIR})
target_link_libraries(avenc-compare PRIVATE avenc_subset_decode ${ECCODES_LIB})
if(TARGET ZLIB::ZLIB)
  target_link_libraries(avenc-compare PRIVATE ZLIB::ZLIB)
else()
  target_link_libraries(avenc-compare PRIVATE ${ZLIB_LIBRARIES})
  target_include_directories(avenc-compare PRIVATE ${ZLIB_INCLUDE_DIRS})
endif()
install(TARGETS avenc-compare RUNTIME DESTINATION ${AVENC_BINDIR})

# Sample-based distribution/quantization/codec evaluator.


install(FILES src/avenc_subset_decode.h DESTINATION include/avenc)
include(CTest)
if(BUILD_TESTING)
  add_executable(avenc_compare_metrics_test tests/avenc_compare_metrics.cpp)
  add_test(NAME avenc_compare_metrics COMMAND avenc_compare_metrics_test)
endif()

if(AVENC_BUILD_PYTHON)
  add_subdirectory(python/avenc)
  if(AVENC_PYTHON_ECCODES)
    install(CODE "execute_process(COMMAND \"${Python_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/packaging/bundle_native.py\" \"\${CMAKE_INSTALL_PREFIX}\" \"${AVENC_ECCODES_ROOT}\" COMMAND_ERROR_IS_FATAL ANY)")
  endif()
endif()
