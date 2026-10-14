/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <JPEGENC.h>
#include <JPEGDEC.h>

extern "C" int jpeg_headers_cpp_enc_first(void)
{
	return (int)(sizeof(JPEGENC) + sizeof(JPEGDEC));
}
