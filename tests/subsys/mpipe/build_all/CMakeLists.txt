cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(build_all)

file(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

if(CONFIG_MPIPE_IMG_JPEG_ENCODER AND CONFIG_MPIPE_IMG_JPEG_DECODER)
  target_sources(app PRIVATE
    src/jpeg_headers_cpp_dec_first.cpp
    src/jpeg_headers_cpp_enc_first.cpp
  )
endif()
