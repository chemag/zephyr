/*
 * Copyright 2026 Meta Platforms, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_dispatch.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/mpipe/mpipe_value.h>

#include <zephyr/mpipe/img/mpipe_img_jpeg_encoder.h>

LOG_MODULE_REGISTER(mpipe_img_jpeg_encoder, CONFIG_MPIPE_LOG_LEVEL);

NET_BUF_POOL_FIXED_DEFINE(mpipe_img_enc_pool, CONFIG_MPIPE_IMG_JPEG_ENCODER_POOL_NUM,
			  CONFIG_MPIPE_IMG_JPEG_ENCODER_MAX_OUT_FRAME_SIZE,
			  sizeof(struct mpipe_buffer_meta), mpipe_buffer_destroy);

static int mpipe_img_jpeg_encoder_out_pool_acquire(struct mpipe_buffer_pool *pool,
						   struct net_buf **buf)
{
	struct net_buf *out;
	struct mpipe_buffer_meta *m;

	__ASSERT_NO_MSG(pool != NULL);
	__ASSERT_NO_MSG(buf != NULL);

	out = net_buf_alloc_len(pool->nb_pool, pool->config.size, K_NO_WAIT);
	if (out == NULL) {
		return -ENOBUFS;
	}

	m = mpipe_buffer_get_meta(out);
	m->pool = pool;
	m->bytes_used = 0;
	m->timestamp = 0;
	m->driver_buf = NULL;
	m->priv = NULL;
	out->len = 0;

	*buf = out;

	return 0;
}

static int mpipe_img_jpeg_encoder_out_pool_release(struct mpipe_buffer_pool *pool,
						   struct net_buf *buf)
{
	ARG_UNUSED(pool);

	if (buf != NULL) {
		struct mpipe_buffer_meta *m = mpipe_buffer_get_meta(buf);

		if (m != NULL) {
			m->bytes_used = 0;
			m->timestamp = 0;
			m->driver_buf = NULL;
			m->priv = NULL;
		}
		buf->len = 0;
	}

	return 0;
}

static void mpipe_img_jpeg_encoder_out_pool_init(struct mpipe_img_jpeg_encoder *enc)
{
	const struct mpipe_buffer_pool_config pool_req = {
		.size = CONFIG_MPIPE_IMG_JPEG_ENCODER_MAX_OUT_FRAME_SIZE,
		.align = 1,
		.min_buffers = CONFIG_MPIPE_IMG_JPEG_ENCODER_POOL_NUM,
		.max_buffers = CONFIG_MPIPE_IMG_JPEG_ENCODER_POOL_NUM,
	};

	mpipe_buffer_pool_init(&enc->out_pool);
	enc->out_pool.nb_pool = &mpipe_img_enc_pool;
	(void)mpipe_buffer_pool_set_req_config(&enc->out_pool, &pool_req);
	enc->out_pool.acquire_buffer = mpipe_img_jpeg_encoder_out_pool_acquire;
	enc->out_pool.release_buffer = mpipe_img_jpeg_encoder_out_pool_release;
	enc->out_pool.started = true;
}

static int mpipe_img_jpeg_encoder_encode_one(struct mpipe_img_jpeg_encoder *enc,
					     struct net_buf *in_buf,
					     struct net_buf *out_buf)
{
	struct mpipe_buffer_meta *in_meta = mpipe_buffer_get_meta(in_buf);
	struct mpipe_buffer_meta *out_meta = mpipe_buffer_get_meta(out_buf);
	JPEGE_IMAGE *jpe = &enc->jpe;
	JPEGENCODE jpec;
	size_t pixels;
	size_t chroma_size = 0U;
	size_t input_size;
	size_t y_size;
	uint32_t chroma_width = 0U;
	uint32_t chroma_height = 0U;
	uint8_t pixel_type;
	int out_sz;
	int rc;

	if (in_meta == NULL || out_meta == NULL || in_meta->bytes_used == 0U) {
		return -ENODATA;
	}
	if (in_meta->bytes_used > in_buf->size) {
		return -EINVAL;
	}
	if (enc->width == 0U || enc->height == 0U || enc->width > INT_MAX ||
	    enc->height > INT_MAX || enc->width > SIZE_MAX / enc->height) {
		LOG_ERR("Invalid frame dimensions (w=%u h=%u)", enc->width, enc->height);
		return -EINVAL;
	}

	pixels = (size_t)enc->width * enc->height;
	y_size = pixels;
	switch (enc->in_pixfmt) {
	case VIDEO_PIX_FMT_RGB565:
		pixel_type = JPEGE_PIXEL_RGB565_LE;
		if (pixels > SIZE_MAX / 2U) {
			return -EOVERFLOW;
		}
		input_size = pixels * 2U;
		break;
	case VIDEO_PIX_FMT_RGB565X:
		pixel_type = JPEGE_PIXEL_RGB565_BE;
		if (pixels > SIZE_MAX / 2U) {
			return -EOVERFLOW;
		}
		input_size = pixels * 2U;
		break;
	case VIDEO_PIX_FMT_YUV420:
	case VIDEO_PIX_FMT_NV12:
		pixel_type = enc->in_pixfmt == VIDEO_PIX_FMT_YUV420 ? JPEGE_PIXEL_YUV420P
								    : JPEGE_PIXEL_NV12;
		if (enc->subsample != JPEGE_SUBSAMPLE_420) {
			return -EINVAL;
		}
		chroma_width = (enc->width + 1U) / 2U;
		chroma_height = (enc->height + 1U) / 2U;
		chroma_size = (size_t)chroma_width * chroma_height;
		if (chroma_size > (SIZE_MAX - y_size) / 2U) {
			return -EOVERFLOW;
		}
		input_size = y_size + chroma_size * 2U;
		break;
	default:
		return -ENOTSUP;
	}

	if (input_size > UINT32_MAX || input_size > in_buf->size ||
	    in_meta->bytes_used < input_size) {
		LOG_ERR("Invalid input size (used=%u required=%zu capacity=%u)",
			in_meta->bytes_used, input_size, in_buf->size);
		return -EINVAL;
	}

	rc = JPEGENC_OpenRAM(jpe, out_buf->data, (int)out_buf->size);
	if (rc != JPEGE_SUCCESS) {
		LOG_ERR("JPEGENC_OpenRAM failed (%d)", rc);
		return -EINVAL;
	}
	rc = JPEGENC_EncodeBegin(jpe, &jpec, (int)enc->width, (int)enc->height, pixel_type,
				 enc->subsample, enc->qfactor);
	if (rc != JPEGE_SUCCESS) {
		LOG_ERR("JPEGENC_EncodeBegin failed (%d)", rc);
		return -EINVAL;
	}

	switch (enc->in_pixfmt) {
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_RGB565X:
		rc = JPEGENC_AddFrame(jpe, &jpec, in_buf->data, (int)enc->width * 2);
		break;
	case VIDEO_PIX_FMT_YUV420:
		rc = JPEGENC_AddFrameYUV420P(jpe, &jpec, in_buf->data, (int)enc->width,
					       &in_buf->data[y_size], (int)chroma_width,
					       &in_buf->data[y_size + chroma_size],
					       (int)chroma_width);
		break;
	case VIDEO_PIX_FMT_NV12:
		rc = JPEGENC_AddFrameNV12(jpe, &jpec, in_buf->data, (int)enc->width,
					    &in_buf->data[y_size], (int)(chroma_width * 2U));
		break;
	default:
		return -ENOTSUP;
	}
	if (rc != JPEGE_SUCCESS) {
		LOG_ERR("JPEG frame encoding failed (%d)", JPEGENC_GetLastError(jpe));
		return -EIO;
	}

	out_sz = JPEGENC_Close(jpe);
	if (out_sz <= 0) {
		LOG_ERR("JPEGENC_Close failed (%d)", JPEGENC_GetLastError(jpe));
		return -EIO;
	}

	out_meta->bytes_used = (uint32_t)out_sz;
	out_meta->timestamp = in_meta->timestamp;
	out_buf->len = (uint16_t)MIN((size_t)out_sz, UINT16_MAX);
	return 0;
}

static int mpipe_img_jpeg_encoder_chain_fn(struct mpipe_pad *pad, struct net_buf *in_buf,
					   struct net_buf **out_buf)
{
	struct mpipe_transform *transform = (struct mpipe_transform *)pad->object.container;
	struct mpipe_img_jpeg_encoder *enc = (struct mpipe_img_jpeg_encoder *)transform;
	struct mpipe_buffer_pool *out_pool = transform->out_pool;
	struct net_buf *cur;
	struct net_buf *next;

	*out_buf = NULL;

	if (out_pool == NULL) {
		out_pool = &enc->out_pool;
	}

	if (in_buf == NULL || out_pool->acquire_buffer == NULL) {
		return -EINVAL;
	}

	cur = in_buf;
	while (cur != NULL) {
		struct net_buf *out = NULL;
		int ret = out_pool->acquire_buffer(out_pool, &out);

		if (ret != 0 || out == NULL) {
			LOG_ERR("Failed to acquire output buffer (%d)", ret);
			net_buf_unref(cur);
			if (*out_buf != NULL) {
				net_buf_unref(*out_buf);
				*out_buf = NULL;
			}
			return -ENOMEM;
		}

		next = cur->frags;
		cur->frags = NULL;

		ret = mpipe_img_jpeg_encoder_encode_one(enc, cur, out);
		if (ret < 0) {
			net_buf_unref(out);
			net_buf_unref(cur);
			if (*out_buf != NULL) {
				net_buf_unref(*out_buf);
				*out_buf = NULL;
			}
			if (next != NULL) {
				net_buf_unref(next);
			}
			return ret;
		}

		if (*out_buf == NULL) {
			*out_buf = out;
		} else {
			net_buf_frag_add(*out_buf, out);
		}

		net_buf_unref(cur);
		cur = next;
	}

	return 0;
}

static const uint32_t jpeg_encoder_in_pixfmts[] = {
	VIDEO_PIX_FMT_RGB565,
	VIDEO_PIX_FMT_RGB565X,
	VIDEO_PIX_FMT_YUV420,
	VIDEO_PIX_FMT_NV12,
};

static int mpipe_img_jpeg_encoder_enum_caps(struct mpipe_pad *pad, uint32_t index,
					    const struct mpipe_structure *filter,
					    struct mpipe_structure *out)
{
	struct mpipe_structure candidate;
	struct mpipe_value fmt;
	int ret;

	fmt.type = MPIPE_TYPE_UINT;
	if (pad->direction == MPIPE_PAD_SINK) {
		if (index >= ARRAY_SIZE(jpeg_encoder_in_pixfmts)) {
			return -ENOENT;
		}
		fmt.v_uint = jpeg_encoder_in_pixfmts[index];
	} else if (pad->direction == MPIPE_PAD_SRC) {
		if (index > 0U) {
			return -ENOENT;
		}
		fmt.v_uint = VIDEO_PIX_FMT_JPEG;
	} else {
		return -EINVAL;
	}

	ret = mpipe_structure_init(&candidate, MPIPE_MEDIA_VIDEO);
	if (ret != 0) {
		return ret;
	}
	ret = mpipe_structure_append_value(&candidate, MPIPE_CAPS_PIXEL_FORMAT, &fmt);
	if (ret != 0) {
		mpipe_structure_clear(&candidate);
		return ret;
	}
	ret = mpipe_pad_enum_filter(&candidate, filter, out);
	mpipe_structure_clear(&candidate);
	return ret;
}

static int mpipe_img_jpeg_encoder_transform_caps(struct mpipe_transform *transform,
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
		if (index > 0U) {
			return -ENOENT;
		}
		fmt.v_uint = VIDEO_PIX_FMT_JPEG;
	} else if (direction == MPIPE_PAD_SINK) {
		if (index >= ARRAY_SIZE(jpeg_encoder_in_pixfmts)) {
			return -ENOENT;
		}
		fmt.v_uint = jpeg_encoder_in_pixfmts[index];
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

static int mpipe_img_jpeg_encoder_set_caps(struct mpipe_transform *transform,
					   enum mpipe_pad_direction direction,
					   const struct mpipe_structure *caps)
{
	struct mpipe_img_jpeg_encoder *enc = (struct mpipe_img_jpeg_encoder *)transform;
	const struct mpipe_value *v;
	uint32_t pixfmt;

	if (caps == NULL) {
		return -EINVAL;
	}

	if (direction == MPIPE_PAD_SINK) {
		v = mpipe_structure_get_value(caps, MPIPE_CAPS_PIXEL_FORMAT);
		if (v == NULL || v->type != MPIPE_TYPE_UINT) {
			return -EINVAL;
		}
		pixfmt = mpipe_value_get_uint(v);
		switch (pixfmt) {
		case VIDEO_PIX_FMT_RGB565:
		case VIDEO_PIX_FMT_RGB565X:
			break;
		case VIDEO_PIX_FMT_YUV420:
		case VIDEO_PIX_FMT_NV12:
			if (enc->subsample != JPEGE_SUBSAMPLE_420) {
				return -EINVAL;
			}
			break;
		default:
			return -ENOTSUP;
		}
		enc->in_pixfmt = pixfmt;

		v = mpipe_structure_get_value(caps, MPIPE_CAPS_IMAGE_WIDTH);
		if (v != NULL && v->type == MPIPE_TYPE_UINT) {
			enc->width = mpipe_value_get_uint(v);
		}

		v = mpipe_structure_get_value(caps, MPIPE_CAPS_IMAGE_HEIGHT);
		if (v != NULL && v->type == MPIPE_TYPE_UINT) {
			enc->height = mpipe_value_get_uint(v);
		}

		return mpipe_pad_set_caps(&transform->sink_pad, caps);
	}

	if (direction == MPIPE_PAD_SRC) {
		return mpipe_pad_set_caps(&transform->src_pad, caps);
	}

	return -EINVAL;
}

static int mpipe_img_jpeg_encoder_decide_buffer_pool(struct mpipe_transform *self,
						     struct mpipe_dispatch *query)
{
	struct mpipe_img_jpeg_encoder *enc = (struct mpipe_img_jpeg_encoder *)self;
	struct mpipe_buffer_pool *down_pool = query->pool;

	self->out_pool = &enc->out_pool;
	if (down_pool != NULL) {
		self->out_pool = down_pool;
	}

	return 0;
}

static int mpipe_img_jpeg_encoder_set_property(struct mpipe_object *obj, uint32_t key,
					       const void *val)
{
	struct mpipe_img_jpeg_encoder *enc = (struct mpipe_img_jpeg_encoder *)obj;

	switch (key) {
	case MPIPE_PROP_IMG_JPEG_ENCODER_QUALITY:
		if ((uintptr_t)val > JPEGE_Q_LOW) {
			return -EINVAL;
		}
		enc->qfactor = (uint8_t)(uintptr_t)val;
		return 0;
	case MPIPE_PROP_IMG_JPEG_ENCODER_SUBSAMPLE:
		if (((uintptr_t)val != JPEGE_SUBSAMPLE_444 &&
		     (uintptr_t)val != JPEGE_SUBSAMPLE_420) ||
		    ((enc->in_pixfmt == VIDEO_PIX_FMT_YUV420 ||
		      enc->in_pixfmt == VIDEO_PIX_FMT_NV12) &&
		     (uintptr_t)val != JPEGE_SUBSAMPLE_420)) {
			return -EINVAL;
		}
		enc->subsample = (uint8_t)(uintptr_t)val;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int mpipe_img_jpeg_encoder_get_property(struct mpipe_object *obj, uint32_t key, void *val)
{
	struct mpipe_img_jpeg_encoder *enc = (struct mpipe_img_jpeg_encoder *)obj;

	switch (key) {
	case MPIPE_PROP_IMG_JPEG_ENCODER_QUALITY:
		*(uint8_t *)val = enc->qfactor;
		return 0;
	case MPIPE_PROP_IMG_JPEG_ENCODER_SUBSAMPLE:
		*(uint8_t *)val = enc->subsample;
		return 0;
	default:
		return -ENOTSUP;
	}
}

int mpipe_img_jpeg_encoder_init(struct mpipe_img_jpeg_encoder *enc, uint8_t id)
{
	__ASSERT_NO_MSG(enc != NULL);

	struct mpipe_transform *transform = &enc->transform;
	struct mpipe_element *self = &transform->element;
	int ret = mpipe_transform_init(transform, id);

	if (ret != 0) {
		return ret;
	}

	mpipe_element_set_name(self, "img_jpeg_encoder");

	enc->subsample = JPEGE_SUBSAMPLE_420;
	enc->qfactor = JPEGE_Q_BEST;
	enc->in_pixfmt = VIDEO_PIX_FMT_RGB565;
	enc->width = 0U;
	enc->height = 0U;

	transform->mode = MPIPE_MODE_NORMAL;
	transform->out_pool = &enc->out_pool;

	transform->sink_pad.enum_caps_fn = mpipe_img_jpeg_encoder_enum_caps;
	transform->src_pad.enum_caps_fn = mpipe_img_jpeg_encoder_enum_caps;

	mpipe_img_jpeg_encoder_out_pool_init(enc);

	self->object.set_property = mpipe_img_jpeg_encoder_set_property;
	self->object.get_property = mpipe_img_jpeg_encoder_get_property;

	transform->set_caps = mpipe_img_jpeg_encoder_set_caps;
	transform->transform_caps = mpipe_img_jpeg_encoder_transform_caps;
	transform->sink_pad.chain_fn = mpipe_img_jpeg_encoder_chain_fn;
	transform->decide_buffer_pool = mpipe_img_jpeg_encoder_decide_buffer_pool;

	return 0;
}
