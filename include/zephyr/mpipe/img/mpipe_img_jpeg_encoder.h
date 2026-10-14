/*
 * Copyright 2026 Meta Platforms, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup mpipe_img_jpeg_encoders
 * @brief Software JPEG encoder transform element.
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_JPEG_ENCODER_H_
#define ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_JPEG_ENCODER_H_

/**
 * @defgroup mpipe_img_jpeg_encoders Encoders
 * @ingroup mpipe_img
 * @brief JPEG encoder elements.
 * @{
 */

#include <zephyr/kernel.h>

#include <JPEGENC.h>
#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_transform.h>

enum mpipe_prop_img_jpeg_encoder {
	MPIPE_PROP_IMG_JPEG_ENCODER_QUALITY = MPIPE_PROP_TRANSFORM_LAST + 1,
	MPIPE_PROP_IMG_JPEG_ENCODER_SUBSAMPLE,
};

struct mpipe_img_jpeg_encoder {
	struct mpipe_transform transform;
	JPEGE_IMAGE jpe;
	struct mpipe_buffer_pool out_pool;
	uint8_t subsample;
	uint8_t qfactor;
	/** Input pixel format (VIDEO_PIX_FMT_*). */
	uint32_t in_pixfmt;
	uint32_t width;
	uint32_t height;
};

int mpipe_img_jpeg_encoder_init(struct mpipe_img_jpeg_encoder *enc, uint8_t id);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_JPEG_ENCODER_H_ */
