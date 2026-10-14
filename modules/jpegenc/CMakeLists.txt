# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_JPEGENC)
  zephyr_library_named(jpegenc)
  zephyr_compile_definitions(JPEGENC_NO_ARDUINO)
  zephyr_include_directories(${ZEPHYR_JPEGENC_MODULE_DIR}/src)
  zephyr_library_sources(${ZEPHYR_JPEGENC_MODULE_DIR}/src/jpegenc.c)
endif()
