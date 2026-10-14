/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/mpipe/img/mpipe_img.h>

#if defined(CONFIG_MPIPE_IMG_JPEG_ENCODER) && defined(CONFIG_MPIPE_IMG_JPEG_DECODER)
int jpeg_headers_c_enc_first(void);
int jpeg_headers_c_dec_first(void);
int jpeg_headers_cpp_enc_first(void);
int jpeg_headers_cpp_dec_first(void);
#endif

int main(void)
{
#if defined(CONFIG_MPIPE_IMG_JPEG_ENCODER) && defined(CONFIG_MPIPE_IMG_JPEG_DECODER)
	(void)jpeg_headers_c_enc_first();
	(void)jpeg_headers_c_dec_first();
	(void)jpeg_headers_cpp_enc_first();
	(void)jpeg_headers_cpp_dec_first();
#endif
	return 0;
}
