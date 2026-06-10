cmake_minimum_required(VERSION 3.16)
project(vc3ds-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release build strips debug info; keep it small for the aarch64 server
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
  -Wall -Wextra
  $<$<CONFIG:Release>:-O2>
)

add_executable(vc3ds-server main.cpp)

# pthreads for std::thread
find_package(Threads REQUIRED)
target_link_libraries(vc3ds-server PRIVATE Threads::Threads)

# Include local Protocol.hpp
target_include_directories(vc3ds-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS vc3ds-server DESTINATION bin)
