/*
 * Copyright 2026 Meta Platforms, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_dispatch.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/mpipe_value.h>

#include <zephyr/mpipe/img/mpipe_img_jpeg_decoder.h>

LOG_MODULE_REGISTER(mpipe_img_jpeg_decoder, CONFIG_MPIPE_LOG_LEVEL);

NET_BUF_POOL_FIXED_DEFINE(mpipe_img_dec_pool, CONFIG_MPIPE_IMG_JPEG_DECODER_POOL_NUM,
			  CONFIG_MPIPE_IMG_JPEG_DECODER_MAX_OUT_FRAME_SIZE,
			  sizeof(struct mpipe_buffer_meta), mpipe_buffer_destroy);

static int mpipe_img_jpeg_decoder_out_pool_acquire(struct mpipe_buffer_pool *pool,
						   struct net_buf **buf)
{
	struct mpipe_buffer_meta *meta;
	struct net_buf *out;

	__ASSERT_NO_MSG(pool != NULL);
	__ASSERT_NO_MSG(buf != NULL);

	out = net_buf_alloc_len(pool->nb_pool, pool->config.size, K_NO_WAIT);
	if (out == NULL) {
		return -ENOBUFS;
	}

	meta = mpipe_buffer_get_meta(out);
	meta->pool = pool;
	meta->bytes_used = 0U;
	meta->timestamp = 0U;
	meta->driver_buf = NULL;
	meta->priv = NULL;
	out->len = 0U;
	*buf = out;

	return 0;
}

static int mpipe_img_jpeg_decoder_out_pool_release(struct mpipe_buffer_pool *pool,
						   struct net_buf *buf)
{
	ARG_UNUSED(pool);

	if (buf != NULL) {
		struct mpipe_buffer_meta *meta = mpipe_buffer_get_meta(buf);

		if (meta != NULL) {
			meta->bytes_used = 0U;
			meta->timestamp = 0U;
			meta->driver_buf = NULL;
			meta->priv = NULL;
		}
		buf->len = 0U;
	}

	return 0;
}

static void mpipe_img_jpeg_decoder_out_pool_init(struct mpipe_img_jpeg_decoder *dec)
{
	const struct mpipe_buffer_pool_config pool_req = {
		.size = CONFIG_MPIPE_IMG_JPEG_DECODER_MAX_OUT_FRAME_SIZE,
		.align = 1U,
		.min_buffers = CONFIG_MPIPE_IMG_JPEG_DECODER_POOL_NUM,
		.max_buffers = CONFIG_MPIPE_IMG_JPEG_DECODER_POOL_NUM,
	};

	mpipe_buffer_pool_init(&dec->out_pool);
	dec->out_pool.nb_pool = &mpipe_img_dec_pool;
	(void)mpipe_buffer_pool_set_req_config(&dec->out_pool, &pool_req);
	dec->out_pool.acquire_buffer = mpipe_img_jpeg_decoder_out_pool_acquire;
	dec->out_pool.release_buffer = mpipe_img_jpeg_decoder_out_pool_release;
	dec->out_pool.started = true;
}

static int mpipe_img_jpeg_decoder_draw(JPEGDRAW *draw)
{
	struct mpipe_img_jpeg_decoder *dec = draw->pUser;
	uint8_t *src = (uint8_t *)draw->pPixels;
	uint8_t *dst;
	size_t row_size;
	size_t src_pitch;
	size_t dst_pitch;

	if (dec == NULL || dec->draw_buf == NULL || draw->pPixels == NULL || draw->iBpp != 16 ||
	    draw->x < 0 || draw->y < 0 || draw->iWidth < 0 || draw->iWidthUsed < 0 ||
	    draw->iHeight < 0 || (uint32_t)(draw->x + draw->iWidthUsed) > dec->width ||
	    (uint32_t)(draw->y + draw->iHeight) > dec->height) {
		if (dec != NULL) {
			dec->draw_error = -EINVAL;
		}
		return 0;
	}

	row_size = (size_t)draw->iWidthUsed * 2U;
	src_pitch = (size_t)draw->iWidth * 2U;
	dst_pitch = (size_t)dec->width * 2U;
	if ((size_t)draw->y * dst_pitch + (size_t)draw->x * 2U + row_size >
	    dec->draw_buf->size ||
	    (draw->iHeight > 0 &&
	     ((size_t)(draw->y + draw->iHeight - 1) * dst_pitch +
	      (size_t)draw->x * 2U + row_size > dec->draw_buf->size))) {
		dec->draw_error = -ENOSPC;
		return 0;
	}

	dst = &dec->draw_buf->data[(size_t)draw->y * dst_pitch + (size_t)draw->x * 2U];
	for (int row = 0; row < draw->iHeight; row++) {
		memcpy(dst, src, row_size);
		dst += dst_pitch;
		src += src_pitch;
	}

	return 1;
}

static int mpipe_img_jpeg_decoder_output_size(const struct mpipe_img_jpeg_decoder *dec,
						       size_t *size)
{
	size_t pixels;
	size_t chroma;

	if (dec->width == 0U || dec->height == 0U || dec->width > SIZE_MAX / dec->height) {
		return -EINVAL;
	}
	pixels = (size_t)dec->width * dec->height;

	switch (dec->out_pixfmt) {
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_RGB565X:
		if (pixels > SIZE_MAX / 2U) {
			return -EOVERFLOW;
		}
		*size = pixels * 2U;
		return 0;
	case VIDEO_PIX_FMT_YUV420:
	case VIDEO_PIX_FMT_NV12:
		chroma = (size_t)((dec->width + 1U) / 2U) * ((dec->height + 1U) / 2U);
		if (chroma > (SIZE_MAX - pixels) / 2U) {
			return -EOVERFLOW;
		}
		*size = pixels + chroma * 2U;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int mpipe_img_jpeg_decoder_error(const JPEGIMAGE *jpg)
{
	switch (JPEGDEC_getLastError((JPEGIMAGE *)jpg)) {
	case JPEG_INVALID_PARAMETER:
	case JPEG_INVALID_FILE:
		return -EINVAL;
	case JPEG_UNSUPPORTED_FEATURE:
		return -ENOTSUP;
	default:
		return -EIO;
	}
}

static int mpipe_img_jpeg_decoder_decode_one(struct mpipe_img_jpeg_decoder *dec,
						     struct net_buf *in_buf,
						     struct net_buf *out_buf)
{
	struct mpipe_buffer_meta *in_meta = mpipe_buffer_get_meta(in_buf);
	struct mpipe_buffer_meta *out_meta = mpipe_buffer_get_meta(out_buf);
	uint32_t in_size = in_meta->bytes_used;
	size_t out_size;
	size_t y_size;
	size_t chroma_size;
	uint32_t chroma_width;
	uint32_t chroma_height;
	JPEG_DRAW_CALLBACK *draw = NULL;
	int ret;

	if (in_size == 0U || in_size > in_buf->size || in_size > INT_MAX) {
		return -EINVAL;
	}
	if (dec->out_pixfmt == VIDEO_PIX_FMT_RGB565 ||
	    dec->out_pixfmt == VIDEO_PIX_FMT_RGB565X) {
		draw = mpipe_img_jpeg_decoder_draw;
	}
	if (JPEGDEC_openRAM(&dec->jpg, in_buf->data, (int)in_size, draw) == 0) {
		LOG_ERR("JPEGDEC_openRAM failed (%d)", JPEGDEC_getLastError(&dec->jpg));
		return mpipe_img_jpeg_decoder_error(&dec->jpg);
	}
	if (JPEGDEC_getWidth(&dec->jpg) <= 0 || JPEGDEC_getHeight(&dec->jpg) <= 0 ||
	    (uint32_t)JPEGDEC_getWidth(&dec->jpg) != dec->width ||
	    (uint32_t)JPEGDEC_getHeight(&dec->jpg) != dec->height) {
		LOG_ERR("JPEG dimensions %dx%d do not match negotiated %ux%u",
			JPEGDEC_getWidth(&dec->jpg), JPEGDEC_getHeight(&dec->jpg), dec->width,
			dec->height);
		JPEGDEC_close(&dec->jpg);
		return -EINVAL;
	}

	ret = mpipe_img_jpeg_decoder_output_size(dec, &out_size);
	if (ret != 0 || out_size > out_buf->size || out_size > UINT32_MAX) {
		JPEGDEC_close(&dec->jpg);
		return (ret != 0) ? ret : -ENOSPC;
	}

	switch (dec->out_pixfmt) {
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_RGB565X:
		JPEGDEC_setPixelType(&dec->jpg, dec->out_pixfmt == VIDEO_PIX_FMT_RGB565
							 ? JPEGD_PIXEL_RGB565_LE
							 : JPEGD_PIXEL_RGB565_BE);
		JPEGDEC_setUserPointer(&dec->jpg, dec);
		dec->draw_buf = out_buf;
		dec->draw_error = 0;
		ret = JPEGDEC_decode(&dec->jpg, 0, 0, 0);
		dec->draw_buf = NULL;
		if (ret == 0 || dec->draw_error != 0) {
			if (dec->draw_error != 0) {
				ret = dec->draw_error;
			} else {
				ret = mpipe_img_jpeg_decoder_error(&dec->jpg);
			}
			JPEGDEC_close(&dec->jpg);
			return ret;
		}
		break;
	case VIDEO_PIX_FMT_YUV420:
		y_size = (size_t)dec->width * dec->height;
		chroma_width = (dec->width + 1U) / 2U;
		chroma_height = (dec->height + 1U) / 2U;
		chroma_size = (size_t)chroma_width * chroma_height;
		ret = JPEGDEC_decodeYUV420P(&dec->jpg, out_buf->data, (int)dec->width,
						 &out_buf->data[y_size], (int)chroma_width,
						 &out_buf->data[y_size + chroma_size],
						 (int)chroma_width);
		if (ret == 0) {
			ret = mpipe_img_jpeg_decoder_error(&dec->jpg);
			JPEGDEC_close(&dec->jpg);
			return ret;
		}
		break;
	case VIDEO_PIX_FMT_NV12:
		y_size = (size_t)dec->width * dec->height;
		chroma_width = (dec->width + 1U) / 2U;
		ret = JPEGDEC_decodeNV12(&dec->jpg, out_buf->data, (int)dec->width,
					    &out_buf->data[y_size], (int)(chroma_width * 2U));
		if (ret == 0) {
			ret = mpipe_img_jpeg_decoder_error(&dec->jpg);
			JPEGDEC_close(&dec->jpg);
			return ret;
		}
		break;
	default:
		JPEGDEC_close(&dec->jpg);
		return -ENOTSUP;
	}

	JPEGDEC_close(&dec->jpg);
	out_meta->bytes_used = (uint32_t)out_size;
	out_meta->timestamp = in_meta->timestamp;
	out_buf->len = (uint16_t)MIN(out_size, UINT16_MAX);
	return 0;
}

static int mpipe_img_jpeg_decoder_chain_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
						   struct net_buf **out_buf)
{
	struct mpipe_transform *transform = (struct mpipe_transform *)pad->object.container;
	struct mpipe_img_jpeg_decoder *dec = (struct mpipe_img_jpeg_decoder *)transform;
	struct mpipe_buffer_pool *out_pool = transform->out_pool;
	struct net_buf *cur = in_buf;
	struct net_buf *next;
	int ret;

	*out_buf = NULL;
	if (out_pool == NULL) {
		out_pool = &dec->out_pool;
	}
	if (cur == NULL || out_pool->acquire_buffer == NULL) {
		return -EINVAL;
	}

	while (cur != NULL) {
		struct net_buf *out = NULL;

		next = cur->frags;
		cur->frags = NULL;
		ret = out_pool->acquire_buffer(out_pool, &out);
		if (ret != 0 || out == NULL) {
			net_buf_unref(cur);
			if (next != NULL) {
				net_buf_unref(next);
			}
			if (*out_buf != NULL) {
				net_buf_unref(*out_buf);
				*out_buf = NULL;
			}
			return (ret != 0) ? ret : -ENOBUFS;
		}

		ret = mpipe_img_jpeg_decoder_decode_one(dec, cur, out);
		net_buf_unref(cur);
		if (ret != 0) {
			net_buf_unref(out);
			if (next != NULL) {
				net_buf_unref(next);
			}
			if (*out_buf != NULL) {
				net_buf_unref(*out_buf);
				*out_buf = NULL;
			}
			return ret;
		}

		if (*out_buf == NULL) {
			*out_buf = out;
		} else {
			net_buf_frag_add(*out_buf, out);
		}
		cur = next;
	}

	return 0;
}

static const uint32_t jpeg_decoder_out_pixfmts[] = {
	VIDEO_PIX_FMT_RGB565,
	VIDEO_PIX_FMT_RGB565X,
	VIDEO_PIX_FMT_YUV420,
	VIDEO_PIX_FMT_NV12,
};

#define JPEG_DECODER_SINK_FIELDS(X) X(MPIPE_CAPS_PIXEL_FORMAT, MPIPE_VALUE_UINT(VIDEO_PIX_FMT_JPEG))

MPIPE_STRUCTURE_DEFINE(jpeg_decoder_sink_caps, MPIPE_MEDIA_VIDEO, JPEG_DECODER_SINK_FIELDS);

static int mpipe_img_jpeg_decoder_enum_caps(struct mpipe_pad *pad, uint32_t index,
						    const struct mpipe_structure *filter,
						    struct mpipe_structure *out)
{
	struct mpipe_structure candidate;
	struct mpipe_value fmt;
	int ret;

	if (pad->direction == MPIPE_PAD_SINK) {
		return (index > 0U) ? -ENOENT
				    : mpipe_pad_enum_filter(&jpeg_decoder_sink_caps, filter, out);
	}
	if (index >= ARRAY_SIZE(jpeg_decoder_out_pixfmts)) {
		return -ENOENT;
	}

	ret = mpipe_structure_init(&candidate, MPIPE_MEDIA_VIDEO);
	if (ret != 0) {
		return ret;
	}
	fmt.type = MPIPE_TYPE_UINT;
	fmt.v_uint = jpeg_decoder_out_pixfmts[index];
	ret = mpipe_structure_append_value(&candidate, MPIPE_CAPS_PIXEL_FORMAT, &fmt);
	if (ret != 0) {
		return ret;
	}
	return mpipe_pad_enum_filter(&candidate, filter, out);
}

static int mpipe_img_jpeg_decoder_transform_caps(struct mpipe_transform *transform,
						 enum mpipe_pad_direction direction,
						 const struct mpipe_structure *in, uint32_t index,
						 struct mpipe_structure *out)
{
	static const uint8_t passthrough[] = {
		MPIPE_CAPS_IMAGE_WIDTH,
		MPIPE_CAPS_IMAGE_HEIGHT,
		MPIPE_CAPS_FRAME_INTERVAL,
	};
	struct mpipe_value fmt;
	int ret;

	ARG_UNUSED(transform);
	if (in == NULL) {
		return -EINVAL;
	}
	if (mpipe_structure_is_any(in)) {
		return (index > 0U) ? -ENOENT : mpipe_structure_init_any(out);
	}

	fmt.type = MPIPE_TYPE_UINT;
	if (direction == MPIPE_PAD_SRC) {
		if (index >= ARRAY_SIZE(jpeg_decoder_out_pixfmts)) {
			return -ENOENT;
		}
		fmt.v_uint = jpeg_decoder_out_pixfmts[index];
	} else if (direction == MPIPE_PAD_SINK) {
		if (index > 0U) {
			return -ENOENT;
		}
		fmt.v_uint = VIDEO_PIX_FMT_JPEG;
	} else {
		return -EINVAL;
	}

	ret = mpipe_structure_init(out, MPIPE_MEDIA_VIDEO);
	if (ret != 0) {
		return ret;
	}
	ret = mpipe_structure_append_value(out, MPIPE_CAPS_PIXEL_FORMAT, &fmt);
	if (ret != 0) {
		mpipe_structure_clear(out);
		return ret;
	}
	for (uint8_t i = 0U; i < ARRAY_SIZE(passthrough); i++) {
		ret = mpipe_structure_copy_field(in, out, passthrough[i]);
		if (ret != 0 && ret != -ENOENT) {
			mpipe_structure_clear(out);
			return ret;
		}
	}
	return 0;
}

static void mpipe_img_jpeg_decoder_read_geometry(struct mpipe_img_jpeg_decoder *dec,
						  const struct mpipe_structure *caps)
{
	const struct mpipe_value *value;

	value = mpipe_structure_get_value(caps, MPIPE_CAPS_IMAGE_WIDTH);
	if (value != NULL && value->type == MPIPE_TYPE_UINT) {
		dec->width = mpipe_value_get_uint(value);
	}
	value = mpipe_structure_get_value(caps, MPIPE_CAPS_IMAGE_HEIGHT);
	if (value != NULL && value->type == MPIPE_TYPE_UINT) {
		dec->height = mpipe_value_get_uint(value);
	}
}

static int mpipe_img_jpeg_decoder_set_caps(struct mpipe_transform *transform,
						   enum mpipe_pad_direction direction,
						   const struct mpipe_structure *caps)
{
	struct mpipe_img_jpeg_decoder *dec = (struct mpipe_img_jpeg_decoder *)transform;
	const struct mpipe_value *value;

	if (caps == NULL) {
		return -EINVAL;
	}
	mpipe_img_jpeg_decoder_read_geometry(dec, caps);
	if (direction == MPIPE_PAD_SINK) {
		return mpipe_pad_set_caps(&transform->sink_pad, caps);
	}
	if (direction == MPIPE_PAD_SRC) {
		value = mpipe_structure_get_value(caps, MPIPE_CAPS_PIXEL_FORMAT);
		if (value != NULL && value->type == MPIPE_TYPE_UINT) {
			dec->out_pixfmt = mpipe_value_get_uint(value);
		}
		return mpipe_pad_set_caps(&transform->src_pad, caps);
	}
	return -EINVAL;
}

static int mpipe_img_jpeg_decoder_decide_buffer_pool(struct mpipe_transform *self,
						      struct mpipe_dispatch *query)
{
	struct mpipe_img_jpeg_decoder *dec = (struct mpipe_img_jpeg_decoder *)self;

	self->out_pool = (query->pool != NULL) ? query->pool : &dec->out_pool;
	return 0;
}

int mpipe_img_jpeg_decoder_init(struct mpipe_img_jpeg_decoder *dec, uint8_t id)
{
	struct mpipe_transform *transform;
	struct mpipe_element *self;
	int ret;

	__ASSERT_NO_MSG(dec != NULL);
	transform = &dec->transform;
	self = &transform->element;
	ret = mpipe_transform_init(transform, id);
	if (ret != 0) {
		return ret;
	}

	mpipe_element_set_name(self, "img_jpeg_decoder");
	dec->out_pixfmt = VIDEO_PIX_FMT_RGB565;
	dec->width = 0U;
	dec->height = 0U;
	dec->draw_buf = NULL;
	dec->draw_error = 0;
	transform->mode = MPIPE_MODE_NORMAL;
	transform->out_pool = &dec->out_pool;
	transform->sink_pad.enum_caps_fn = mpipe_img_jpeg_decoder_enum_caps;
	transform->src_pad.enum_caps_fn = mpipe_img_jpeg_decoder_enum_caps;
	mpipe_img_jpeg_decoder_out_pool_init(dec);
	transform->set_caps = mpipe_img_jpeg_decoder_set_caps;
	transform->transform_caps = mpipe_img_jpeg_decoder_transform_caps;
	transform->sink_pad.chain_fn = mpipe_img_jpeg_decoder_chain_fn;
	transform->decide_buffer_pool = mpipe_img_jpeg_decoder_decide_buffer_pool;
	return 0;
}
