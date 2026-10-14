/*
 * Copyright 2026 Meta Platforms, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mpipe_img_jpeg_decoders
 * @brief Software JPEG decoder transform element.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_JPEG_DECODER_H_
#define ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_JPEG_DECODER_H_

/**
 * @defgroup mpipe_img_jpeg_decoders JPEG decoders
 * @ingroup mpipe_img
 * @brief JPEG decoder elements.
 * @{
 */

#include <JPEGDEC.h>

#include <zephyr/mpipe/mpipe_transform.h>

/**
 * @brief Software JPEG decoder element.
 */
struct mpipe_img_jpeg_decoder {
	/** Base transform element */
	struct mpipe_transform transform;
	/** JPEGDEC decoder state */
	JPEGIMAGE jpg;
	/** Output pixel format (VIDEO_PIX_FMT_*) */
	uint32_t out_pixfmt;
	/** Negotiated image width */
	uint32_t width;
	/** Negotiated image height */
	uint32_t height;
	/** Output buffer used by the RGB draw callback */
	struct net_buf *draw_buf;
	/** Error detected by the RGB draw callback */
	int draw_error;
	/** Internal output pool */
	struct mpipe_buffer_pool out_pool;
};

/**
 * @brief Initialize a software JPEG decoder element.
 *
 * @param dec Pointer to the element.
 * @param id Unique element identifier.
 *
 * @retval 0 Success.
 * @return Negative errno on failure.
 */
int mpipe_img_jpeg_decoder_init(struct mpipe_img_jpeg_decoder *dec, uint8_t id);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_JPEG_DECODER_H_ */
