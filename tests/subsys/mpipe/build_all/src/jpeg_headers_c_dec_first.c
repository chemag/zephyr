/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(CONFIG_MPIPE_IMG_JPEG_ENCODER) && defined(CONFIG_MPIPE_IMG_JPEG_DECODER)
#include <JPEGDEC.h>
#include <JPEGENC.h>

int jpeg_headers_c_dec_first(void)
{
	return (int)(sizeof(JPEGE_IMAGE) + sizeof(JPEGIMAGE));
}
#endif
