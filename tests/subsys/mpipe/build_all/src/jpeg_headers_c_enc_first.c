/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(CONFIG_MPIPE_IMG_JPEG_ENCODER) && defined(CONFIG_MPIPE_IMG_JPEG_DECODER)
#include <JPEGENC.h>
#include <JPEGDEC.h>

int jpeg_headers_c_enc_first(void)
{
	return (int)(sizeof(JPEGE_IMAGE) + sizeof(JPEGIMAGE));
}
#endif
