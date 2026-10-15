cmake_minimum_required(VERSION 3.16)
project(thumbnailer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TEST_THUMBNAILER "Build test_thumbnailer CLI" ON)

if(NOT SBS_PREFIX)
  if(DEFINED ENV{SBS_PREFIX} AND NOT "$ENV{SBS_PREFIX}" STREQUAL "")
    set(SBS_PREFIX "$ENV{SBS_PREFIX}")
  else()
    set(SBS_PREFIX "$ENV{HOME}/sbs/_/x86_64-linux-gnu")
  endif()
endif()
set(SBS_PREFIX "${SBS_PREFIX}" CACHE PATH "simplybs host prefix")

find_library(AVFORMAT_LIB avformat PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(AVCODEC_LIB avcodec PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(AVUTIL_LIB avutil PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(SWSCALE_LIB swscale PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(SWRESAMPLE_LIB swresample PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_library(ZLIB_LIB z PATHS "${SBS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT ZLIB_LIB)
  find_library(ZLIB_LIB z REQUIRED)
endif()

include_directories(
  "${CMAKE_SOURCE_DIR}/include"
  "${SBS_PREFIX}/include"
)

add_library(thumbnailer SHARED
  src/abi.cpp
  src/jobs.cpp
  src/thumbnail.cpp
)

target_link_libraries(thumbnailer PRIVATE
  ${AVFORMAT_LIB}
  ${AVCODEC_LIB}
  ${SWSCALE_LIB}
  ${SWRESAMPLE_LIB}
  ${AVUTIL_LIB}
  ${ZLIB_LIB}
)

if(APPLE)
  set_target_properties(thumbnailer PROPERTIES
    OUTPUT_NAME "thumbnailer"
    PREFIX "lib"
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_RPATH ON
    BUILD_RPATH "${SBS_PREFIX}/lib"
    INSTALL_RPATH "@rpath;${SBS_PREFIX}/lib"
  )
  target_link_libraries(thumbnailer PRIVATE
    "-framework CoreFoundation"
    "-framework CoreVideo"
    "-framework CoreMedia"
  )
  if(EXISTS "${CMAKE_SOURCE_DIR}/exports.txt")
    target_link_libraries(thumbnailer PRIVATE "-Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/exports.txt")
  endif()
elseif(ANDROID)
  set_target_properties(thumbnailer PROPERTIES
    OUTPUT_NAME "thumbnailer"
    PREFIX "lib"
    BUILD_WITH_INSTALL_RPATH ON
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
  )
  target_link_libraries(thumbnailer PRIVATE android log mediandk atomic m dl)
  target_link_options(thumbnailer PRIVATE "-Wl,--version-script,${CMAKE_SOURCE_DIR}/exports.map")
else()
  target_link_libraries(thumbnailer PRIVATE pthread dl m)
  # GNU ld turns pushq func@GOTPCREL(%rip) into addr32 call, which
  # invokes FFmpeg's seek callback instead of passing its address.
  target_link_options(thumbnailer PRIVATE
    "-static-libstdc++" "-static-libgcc" "-Wl,--no-relax"
    "-Wl,--version-script,${CMAKE_SOURCE_DIR}/exports.map"
  )
  set_target_properties(thumbnailer PROPERTIES
    OUTPUT_NAME "thumbnailer"
    PREFIX "lib"
    BUILD_WITH_INSTALL_RPATH ON
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
  )
endif()

if(BUILD_TEST_THUMBNAILER AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT ANDROID)
  add_executable(test_thumbnailer tools/test_thumbnailer.cpp)
  target_link_libraries(test_thumbnailer PRIVATE thumbnailer)
  if(NOT APPLE)
    target_link_libraries(test_thumbnailer PRIVATE dl)
  endif()
endif()

install(TARGETS thumbnailer LIBRARY DESTINATION lib)
