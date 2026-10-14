/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <JPEGDEC.h>
#include <JPEGENC.h>

extern "C" int jpeg_headers_cpp_dec_first(void)
{
	return (int)(sizeof(JPEGENC) + sizeof(JPEGDEC));
}
