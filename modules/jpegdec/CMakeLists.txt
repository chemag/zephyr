# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_JPEGDEC)
  zephyr_library_named(jpegdec)
  zephyr_compile_definitions(JPEGDEC_NO_ARDUINO)
  zephyr_include_directories(${ZEPHYR_JPEGDEC_MODULE_DIR}/src)
  zephyr_library_sources(${ZEPHYR_JPEGDEC_MODULE_DIR}/src/jpegdec.c)
endif()
