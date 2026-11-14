cmake_minimum_required(VERSION 3.16)
project(minivectordb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(minivectordb
  src/main.cpp
  src/common.cpp
  src/parser.cpp
  src/database.cpp
  src/executor.cpp
  src/jsonl.cpp
  src/ann/ann_q7.cpp
)

target_compile_features(minivectordb PRIVATE cxx_std_20)
target_compile_options(minivectordb PRIVATE
  -O3
  -Wall
  -Wextra
  -Wpedantic
  -Wno-unused-parameter
)
