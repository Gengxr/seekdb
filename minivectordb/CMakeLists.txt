cmake_minimum_required(VERSION 3.16)
project(minivectordb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(minivectordb
  src/main.cpp
  src/parser.cpp
  src/storage.cpp
  src/exec.cpp
)

target_compile_options(minivectordb PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)

# Prefer a self-contained runtime so the final image needs nothing installed.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_options(minivectordb PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS minivectordb RUNTIME DESTINATION bin)
