# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mpipe_jpeg_decoder_file)

target_sources(app PRIVATE src/main.c)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test_fs)

target_compile_definitions(app PRIVATE
  TEST_FS_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_fs"
)
