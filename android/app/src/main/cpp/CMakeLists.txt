cmake_minimum_required(VERSION 3.22)
project(nodosumi_android C)

# the app drives the libretro core, built from the repo root
set(NODOSUMI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(BUILD_LIBRETRO ON CACHE BOOL "" FORCE)
set(A64_JIT ON CACHE BOOL "" FORCE)
add_subdirectory(${NODOSUMI_ROOT} nodosumi)

add_library(nodosumi_android SHARED frontend.c)
target_include_directories(nodosumi_android PRIVATE ${NODOSUMI_ROOT}/deps/libretro/include)
# the core is a finished shared library, take nothing from it but the link.
# its include paths carry glad's old khrplatform.h, which breaks the ndk's
# egl and gles headers
set_target_properties(nodosumi PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES ""
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ""
  INTERFACE_LINK_LIBRARIES ""
  # android only extracts and loads lib*.so
  PREFIX "lib")
target_link_libraries(nodosumi_android nodosumi EGL GLESv3 aaudio android log)
