cmake_minimum_required(VERSION 3.20)
project(mtl_encode_sdk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MTL_SDK_USE_ST_API "Use legacy st_init/st_start API names (ON) or newer mtl_init/mtl_start (OFF)" ON)

# MTL is required. Default: /home/aa/Media-Transport-Library.
# Override via: cmake .. -DMTL_ROOT=/path/to/MTL
# MTL typically has include/ and build/ or lib/; DPDK is linked by MTL.
set(MTL_ROOT "/home/aa/Media-Transport-Library" CACHE PATH "Root of Media-Transport-Library (override with -DMTL_ROOT=...)")
set(DPDK_ROOT "/home/dpdk" CACHE PATH "Optional: root of DPDK install if MTL does not embed it (e.g. /home/DPDK)")

# ---- mtl_sdk (MTL backend only, no mock) ----
add_library(mtl_sdk STATIC
  mtl_sdk/src/sdp.cpp
  mtl_sdk/src/mtl_sdk.cpp
  mtl_sdk/src/mtl_backend_mtl.cpp
)

target_compile_definitions(mtl_sdk PRIVATE MTL_SDK_WITH_MTL=1)
target_include_directories(mtl_sdk PUBLIC mtl_sdk/include)

if(NOT MTL_ROOT STREQUAL "")
  target_include_directories(mtl_sdk PRIVATE ${MTL_ROOT}/include)
  if(EXISTS "${MTL_ROOT}/build")
    target_include_directories(mtl_sdk PRIVATE ${MTL_ROOT}/build)
    target_link_directories(mtl_sdk PRIVATE ${MTL_ROOT}/build)
  elseif(EXISTS "${MTL_ROOT}/lib")
    target_include_directories(mtl_sdk PRIVATE ${MTL_ROOT}/lib)
    target_link_directories(mtl_sdk PRIVATE ${MTL_ROOT}/lib)
  endif()
  target_link_libraries(mtl_sdk PRIVATE mtl)
endif()
  # Optional: only if MTL was built without embedding DPDK and link fails with undefined rte_*.
if(DPDK_ROOT AND NOT DPDK_ROOT STREQUAL "")
  target_link_directories(mtl_sdk PRIVATE ${DPDK_ROOT}/build/lib)
  # Adjust library names to match your DPDK build (e.g. lib/rte_build/lib/*.a).
  target_link_libraries(mtl_sdk PRIVATE rte_eal rte_ring rte_mempool rte_ethdev rte_kvargs)
endif()

if(MTL_SDK_USE_ST_API)
  target_compile_definitions(mtl_sdk PUBLIC MTL_SDK_USE_ST_API=1)
endif()

# ---- encode_sdk ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVUTIL   REQUIRED libavutil)
pkg_check_modules(SWSCALE  REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

add_library(encode_sdk STATIC
  encode_sdk/src/encode_sdk.cpp
  encode_sdk/src/ffmpeg_utils.cpp
)

target_compile_definitions(encode_sdk PRIVATE __STDC_CONSTANT_MACROS)
target_include_directories(encode_sdk PUBLIC encode_sdk/include)
target_include_directories(encode_sdk PRIVATE
  ${AVFORMAT_INCLUDE_DIRS}
  ${AVCODEC_INCLUDE_DIRS}
  ${AVUTIL_INCLUDE_DIRS}
  ${SWSCALE_INCLUDE_DIRS}
  ${SWRESAMPLE_INCLUDE_DIRS}
)
target_link_libraries(encode_sdk PRIVATE
  mtl_sdk
  ${AVFORMAT_LIBRARIES}
  ${AVCODEC_LIBRARIES}
  ${AVUTIL_LIBRARIES}
  ${SWSCALE_LIBRARIES}
  ${SWRESAMPLE_LIBRARIES}
)

# ---- samples ----
add_executable(st2110_record samples/st2110_record.cpp)
target_link_libraries(st2110_record PRIVATE mtl_sdk encode_sdk)

add_executable(st2110_send samples/st2110_send.cpp)
target_link_libraries(st2110_send PRIVATE mtl_sdk)

# IS-05 receiver daemon: reads connection_state.json, creates MTL RX and encodes to MP4
add_executable(is05_receiver_daemon samples/is05_receiver_daemon.cpp)
target_link_libraries(is05_receiver_daemon PRIVATE mtl_sdk encode_sdk)

# ---- tests ----
add_subdirectory(tests)
