/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Image plugin.
 * @ingroup mpipe_img
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_H_
#define ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_H_

/**
 * @defgroup mpipe_img Image Codecs
 * @ingroup mpipe_plugins
 * @brief Elements that frame, encode, and decode still images.
 *
 * The image plugin is the common namespace for image parsers, encoders, and
 * decoders. Parsers recover complete images from byte streams, while codec
 * elements consume and produce complete image buffers. Stream timing and
 * container policy remain outside the image codecs.
 *
 * Each parser and codec is independently selectable. Enabling the image plugin
 * alone does not select a codec implementation or reserve codec buffers.
 */

#endif /* ZEPHYR_INCLUDE_MPIPE_IMG_MPIPE_IMG_H_ */
