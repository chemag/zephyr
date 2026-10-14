/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/video.h>
#include <zephyr/fs/fs.h>
#include <zephyr/mpipe/base/mpipe_caps_filter.h>
#include <zephyr/mpipe/fs/mpipe_file_sink.h>
#include <zephyr/mpipe/fs/mpipe_file_src.h>
#include <zephyr/mpipe/img/mpipe_img_jpeg_decoder.h>
#include <zephyr/mpipe/img/mpipe_img_jpeg_encoder.h>
#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_message.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "test_jpeg_17x17.h"

#define TEST_MOUNT_POINT "/test_fs"
#define TEST_WIDTH       17U
#define TEST_HEIGHT      17U
#define TEST_RGB_SIZE    (TEST_WIDTH * TEST_HEIGHT * 2U)
#define TEST_CHROMA_SIZE (((TEST_WIDTH + 1U) / 2U) * ((TEST_HEIGHT + 1U) / 2U))
#define TEST_YUV_SIZE    (TEST_WIDTH * TEST_HEIGHT + TEST_CHROMA_SIZE * 2U)
#define TEST_JPEG_SIZE   4096U
#define TEST_RAW_SIZE    MAX(TEST_RGB_SIZE, TEST_YUV_SIZE)

ZBUS_MSG_SUBSCRIBER_DEFINE(mpipe_jpeg_codec_subscriber);

enum {
	PIPE_ID,
	FILE_SRC_ID,
	JPEG_ENCODER_ID,
	JPEG_DECODER_ID,
	CAPS_FILTER_ID,
	FILE_SINK_ID,
};

static struct fs_mount_t test_mount = {
	.type = FS_NATIVE_MOUNT,
	.mnt_point = TEST_MOUNT_POINT,
	.fs_data = TEST_FS_DIR,
};

static struct mpipe pipeline;
static struct mpipe_file_src file_src;
static struct mpipe_img_jpeg_encoder jpeg_encoder;
static struct mpipe_img_jpeg_decoder jpeg_decoder;
static struct mpipe_caps_filter caps_filter;
static struct mpipe_file_sink file_sink;
static uint8_t raw_input[TEST_RAW_SIZE];
static uint8_t output[TEST_JPEG_SIZE];
static uint8_t decoded_yuv[TEST_YUV_SIZE];

static int write_file(const char *path, const uint8_t *data, size_t size)
{
	struct fs_file_t file;
	size_t offset = 0U;
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret != 0) {
		return ret;
	}
	while (offset < size) {
		ssize_t written = fs_write(&file, data + offset, size - offset);

		if (written <= 0) {
			ret = (written < 0) ? (int)written : -EIO;
			goto close;
		}
		offset += (size_t)written;
	}
	ret = 0;

close:
	if (fs_close(&file) != 0 && ret == 0) {
		ret = -EIO;
	}
	return ret;
}

static int read_file(const char *path, uint8_t *data, size_t capacity, size_t *size)
{
	struct fs_file_t file;
	size_t offset = 0U;
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret != 0) {
		return ret;
	}
	while (offset < capacity) {
		ssize_t bytes_read = fs_read(&file, data + offset, capacity - offset);

		if (bytes_read < 0) {
			ret = (int)bytes_read;
			goto close;
		}
		if (bytes_read == 0) {
			break;
		}
		offset += (size_t)bytes_read;
	}
	*size = offset;
	ret = 0;

close:
	if (fs_close(&file) != 0 && ret == 0) {
		ret = -EIO;
	}
	return ret;
}

static uint8_t test_color(uint32_t x, uint32_t y)
{
	if (x == TEST_WIDTH / 2U || y == TEST_HEIGHT / 2U) {
		return 0U;
	}
	if (x < TEST_WIDTH / 2U && y < TEST_HEIGHT / 2U) {
		return 1U;
	}
	if (x >= TEST_WIDTH / 2U && y < TEST_HEIGHT / 2U) {
		return 2U;
	}
	if (x < TEST_WIDTH / 2U) {
		return 3U;
	}
	return 4U;
}

static size_t fill_raw_frame(uint32_t pixfmt)
{
	static const uint16_t rgb565[] = {0x0000U, 0xf800U, 0x07e0U, 0x001fU, 0xffffU};
	static const uint8_t luma[] = {16U, 76U, 150U, 29U, 235U};
	static const uint8_t cb[] = {128U, 85U, 44U, 255U, 128U};
	static const uint8_t cr[] = {128U, 255U, 21U, 107U, 128U};
	const uint32_t chroma_width = (TEST_WIDTH + 1U) / 2U;
	const uint32_t chroma_height = (TEST_HEIGHT + 1U) / 2U;
	const size_t y_size = TEST_WIDTH * TEST_HEIGHT;

	memset(raw_input, 0, sizeof(raw_input));
	if (pixfmt == VIDEO_PIX_FMT_RGB565 || pixfmt == VIDEO_PIX_FMT_RGB565X) {
		for (uint32_t y = 0U; y < TEST_HEIGHT; y++) {
			for (uint32_t x = 0U; x < TEST_WIDTH; x++) {
				uint16_t pixel = rgb565[test_color(x, y)];
				size_t offset = ((size_t)y * TEST_WIDTH + x) * 2U;

				if (pixfmt == VIDEO_PIX_FMT_RGB565) {
					raw_input[offset] = (uint8_t)pixel;
					raw_input[offset + 1U] = (uint8_t)(pixel >> 8);
				} else {
					raw_input[offset] = (uint8_t)(pixel >> 8);
					raw_input[offset + 1U] = (uint8_t)pixel;
				}
			}
		}
		return TEST_RGB_SIZE;
	}

	for (uint32_t y = 0U; y < TEST_HEIGHT; y++) {
		for (uint32_t x = 0U; x < TEST_WIDTH; x++) {
			raw_input[(size_t)y * TEST_WIDTH + x] = luma[test_color(x, y)];
		}
	}
	for (uint32_t y = 0U; y < chroma_height; y++) {
		for (uint32_t x = 0U; x < chroma_width; x++) {
			uint8_t color = test_color(MIN(x * 2U, TEST_WIDTH - 1U),
						   MIN(y * 2U, TEST_HEIGHT - 1U));
			size_t offset = (size_t)y * chroma_width + x;

			if (pixfmt == VIDEO_PIX_FMT_YUV420) {
				raw_input[y_size + offset] = cb[color];
				raw_input[y_size + TEST_CHROMA_SIZE + offset] = cr[color];
			} else {
				raw_input[y_size + offset * 2U] = cb[color];
				raw_input[y_size + offset * 2U + 1U] = cr[color];
			}
		}
	}
	return TEST_YUV_SIZE;
}

static int get_jpeg_dimensions(const uint8_t *jpeg, size_t size, uint16_t *width,
			       uint16_t *height)
{
	for (size_t i = 0U; i + 8U < size; i++) {
		if (jpeg[i] == 0xffU && jpeg[i + 1U] == 0xc0U) {
			*height = ((uint16_t)jpeg[i + 5U] << 8) | jpeg[i + 6U];
			*width = ((uint16_t)jpeg[i + 7U] << 8) | jpeg[i + 8U];
			return 0;
		}
	}
	return -ENOENT;
}

static void check_rgb565(const uint8_t *data, uint32_t pixfmt)
{
	static const uint16_t masks[] = {0xf800U, 0x07e0U, 0x001fU, 0xffffU};
	static const uint32_t x[] = {3U, 13U, 3U, 13U};
	static const uint32_t y[] = {3U, 3U, 13U, 13U};

	for (size_t i = 0U; i < ARRAY_SIZE(masks); i++) {
		size_t offset = ((size_t)y[i] * TEST_WIDTH + x[i]) * 2U;
		uint16_t pixel;

		if (pixfmt == VIDEO_PIX_FMT_RGB565) {
			pixel = (uint16_t)data[offset] | ((uint16_t)data[offset + 1U] << 8);
		} else {
			pixel = ((uint16_t)data[offset] << 8) | (uint16_t)data[offset + 1U];
		}
		zassert_true((pixel & masks[i]) != 0U, "RGB565 quadrant %zu: 0x%04x", i,
			     pixel);
	}
}

static void check_luma(const uint8_t *data)
{
	uint8_t red = data[3U * TEST_WIDTH + 3U];
	uint8_t green = data[3U * TEST_WIDTH + 13U];
	uint8_t blue = data[13U * TEST_WIDTH + 3U];
	uint8_t white = data[13U * TEST_WIDTH + 13U];

	zassert_between_inclusive(red, 30U, 130U, "red luma: %u", red);
	zassert_between_inclusive(green, 90U, 210U, "green luma: %u", green);
	zassert_between_inclusive(blue, 0U, 110U, "blue luma: %u", blue);
	zassert_between_inclusive(white, 170U, 255U, "white luma: %u", white);
}

static void get_chroma(const uint8_t *data, uint32_t pixfmt, uint32_t x, uint32_t y,
		       uint8_t *cb, uint8_t *cr)
{
	const size_t y_size = TEST_WIDTH * TEST_HEIGHT;
	const uint32_t chroma_width = (TEST_WIDTH + 1U) / 2U;
	size_t offset = (size_t)(y / 2U) * chroma_width + x / 2U;

	if (pixfmt == VIDEO_PIX_FMT_YUV420) {
		*cb = data[y_size + offset];
		*cr = data[y_size + TEST_CHROMA_SIZE + offset];
	} else {
		*cb = data[y_size + offset * 2U];
		*cr = data[y_size + offset * 2U + 1U];
	}
}

static void check_chroma(const uint8_t *data, uint32_t pixfmt)
{
	uint8_t red_cb;
	uint8_t red_cr;
	uint8_t blue_cb;
	uint8_t blue_cr;

	get_chroma(data, pixfmt, 3U, 3U, &red_cb, &red_cr);
	get_chroma(data, pixfmt, 3U, 13U, &blue_cb, &blue_cr);
	zassert_true(red_cr > red_cb + 30U, "red chroma: Cb=%u Cr=%u", red_cb, red_cr);
	zassert_true(blue_cb > blue_cr + 30U, "blue chroma: Cb=%u Cr=%u", blue_cb, blue_cr);
}

static void check_jpeg_content(uint8_t *jpeg, size_t size)
{
	const size_t y_size = TEST_WIDTH * TEST_HEIGHT;
	JPEGIMAGE decoder;
	int ret;

	memset(decoded_yuv, 0, sizeof(decoded_yuv));
	ret = JPEGDEC_openRAM(&decoder, jpeg, (int)size, NULL);
	zassert_true(ret != 0, "could not open re-encoded JPEG");
	ret = JPEGDEC_decodeYUV420P(&decoder, decoded_yuv, TEST_WIDTH, &decoded_yuv[y_size],
				    (TEST_WIDTH + 1U) / 2U,
				    &decoded_yuv[y_size + TEST_CHROMA_SIZE],
				    (TEST_WIDTH + 1U) / 2U);
	JPEGDEC_close(&decoder);
	zassert_true(ret != 0, "could not decode re-encoded JPEG");
	check_luma(decoded_yuv);
	check_chroma(decoded_yuv, VIDEO_PIX_FMT_YUV420);
}

static void init_elements(void)
{
	memset(&pipeline, 0, sizeof(pipeline));
	memset(&file_src, 0, sizeof(file_src));
	memset(&jpeg_encoder, 0, sizeof(jpeg_encoder));
	memset(&jpeg_decoder, 0, sizeof(jpeg_decoder));
	memset(&caps_filter, 0, sizeof(caps_filter));
	memset(&file_sink, 0, sizeof(file_sink));

	zassert_ok(mpipe_pipeline_init(&pipeline, PIPE_ID));
	zassert_ok(mpipe_file_src_init(&file_src, FILE_SRC_ID));
	zassert_ok(mpipe_img_jpeg_encoder_init(&jpeg_encoder, JPEG_ENCODER_ID));
	zassert_ok(mpipe_img_jpeg_decoder_init(&jpeg_decoder, JPEG_DECODER_ID));
	zassert_ok(mpipe_caps_filter_init(&caps_filter, CAPS_FILTER_ID));
	zassert_ok(mpipe_file_sink_init(&file_sink, FILE_SINK_ID));
}

static const char *codec_input_path(bool raw_first, uint32_t pixfmt)
{
	if (!raw_first) {
		return TEST_MOUNT_POINT "/decoder-encoder.input.17x17.jpg";
	}

	switch (pixfmt) {
	case VIDEO_PIX_FMT_RGB565:
		return TEST_MOUNT_POINT "/encoder-decoder.input.17x17.rgb565.raw";
	case VIDEO_PIX_FMT_RGB565X:
		return TEST_MOUNT_POINT "/encoder-decoder.input.17x17.rgb565x.raw";
	case VIDEO_PIX_FMT_YUV420:
		return TEST_MOUNT_POINT "/encoder-decoder.input.17x17.yuv420p.raw";
	case VIDEO_PIX_FMT_NV12:
		return TEST_MOUNT_POINT "/encoder-decoder.input.17x17.nv12.raw";
	default:
		return NULL;
	}
}

static const char *codec_output_path(bool raw_first, uint32_t pixfmt)
{
	if (raw_first) {
		switch (pixfmt) {
		case VIDEO_PIX_FMT_RGB565:
			return TEST_MOUNT_POINT "/encoder-decoder.output.17x17.rgb565.raw";
		case VIDEO_PIX_FMT_RGB565X:
			return TEST_MOUNT_POINT "/encoder-decoder.output.17x17.rgb565x.raw";
		case VIDEO_PIX_FMT_YUV420:
			return TEST_MOUNT_POINT "/encoder-decoder.output.17x17.yuv420p.raw";
		case VIDEO_PIX_FMT_NV12:
			return TEST_MOUNT_POINT "/encoder-decoder.output.17x17.nv12.raw";
		default:
			return NULL;
		}
	}

	switch (pixfmt) {
	case VIDEO_PIX_FMT_RGB565:
		return TEST_MOUNT_POINT "/decoder-encoder.output.17x17.rgb565.jpg";
	case VIDEO_PIX_FMT_YUV420:
		return TEST_MOUNT_POINT "/decoder-encoder.output.17x17.yuv420p.jpg";
	case VIDEO_PIX_FMT_NV12:
		return TEST_MOUNT_POINT "/decoder-encoder.output.17x17.nv12.jpg";
	default:
		return NULL;
	}
}

static void run_codec_pipeline(bool raw_first, uint32_t pixfmt)
{
	struct mpipe_structure src_caps;
	struct mpipe_structure raw_caps;
	const struct zbus_channel *channel;
	struct zbus_channel *bus;
	struct mpipe_message message;
	uint32_t block_size;
	uint16_t width = 0U;
	uint16_t height = 0U;
	size_t input_size;
	size_t output_size = 0U;
	const char *input_path = codec_input_path(raw_first, pixfmt);
	const char *output_path = codec_output_path(raw_first, pixfmt);
	const char *input_name;
	const char *output_name;

	zassert_not_null(input_path, "unsupported test format");
	zassert_not_null(output_path, "unsupported test format");
	input_name = strrchr(input_path, '/');
	output_name = strrchr(output_path, '/');
	zassert_not_null(input_name, "invalid input path");
	zassert_not_null(output_name, "invalid output path");
	input_name++;
	output_name++;

	(void)fs_unlink(input_path);
	(void)fs_unlink(output_path);
	if (raw_first) {
		input_size = fill_raw_frame(pixfmt);
		zassert_ok(write_file(input_path, raw_input, input_size));
		block_size = input_size;
	} else {
		zassert_ok(write_file(input_path, jpeg_17x17, sizeof(jpeg_17x17)));
		block_size = sizeof(jpeg_17x17);
	}

	init_elements();
	zassert_ok(mpipe_structure_init_fields(
		&raw_caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT, pixfmt,
		MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT, TEST_WIDTH, MPIPE_CAPS_IMAGE_HEIGHT,
		MPIPE_TYPE_UINT, TEST_HEIGHT, MPIPE_CAPS_END));
	zassert_ok(mpipe_structure_init_fields(
		&src_caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT,
		raw_first ? pixfmt : VIDEO_PIX_FMT_JPEG, MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT,
		TEST_WIDTH, MPIPE_CAPS_IMAGE_HEIGHT, MPIPE_TYPE_UINT, TEST_HEIGHT, MPIPE_CAPS_END));
	zassert_ok(mpipe_pad_set_caps(&file_src.src.src_pad, &src_caps));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&caps_filter,
					       MPIPE_PROP_BASE_CAPS_FILTER_CAPS, &raw_caps,
					       MPIPE_PROP_LIST_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&file_src,
					       MPIPE_PROP_FS_SRC_PATH, input_path,
					       MPIPE_PROP_FS_SRC_BLOCKSIZE, &block_size,
					       MPIPE_PROP_LIST_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&file_sink,
					       MPIPE_PROP_FS_SINK_PATH, output_path,
					       MPIPE_PROP_LIST_END));

	if (raw_first) {
		zassert_ok(mpipe_bin_add((struct mpipe_bin *)&pipeline,
					 (struct mpipe_element *)&file_src,
					 (struct mpipe_element *)&jpeg_encoder,
					 (struct mpipe_element *)&jpeg_decoder,
					 (struct mpipe_element *)&caps_filter,
					 (struct mpipe_element *)&file_sink, NULL));
		zassert_ok(mpipe_element_link((struct mpipe_element *)&file_src,
					    (struct mpipe_element *)&jpeg_encoder,
					    (struct mpipe_element *)&jpeg_decoder,
					    (struct mpipe_element *)&caps_filter,
					    (struct mpipe_element *)&file_sink, NULL));
	} else {
		zassert_ok(mpipe_bin_add((struct mpipe_bin *)&pipeline,
					 (struct mpipe_element *)&file_src,
					 (struct mpipe_element *)&jpeg_decoder,
					 (struct mpipe_element *)&caps_filter,
					 (struct mpipe_element *)&jpeg_encoder,
					 (struct mpipe_element *)&file_sink, NULL));
		zassert_ok(mpipe_element_link((struct mpipe_element *)&file_src,
					    (struct mpipe_element *)&jpeg_decoder,
					    (struct mpipe_element *)&caps_filter,
					    (struct mpipe_element *)&jpeg_encoder,
					    (struct mpipe_element *)&file_sink, NULL));
	}

	bus = mpipe_element_get_bus_chan((struct mpipe_element *)&pipeline);
	zassert_ok(zbus_chan_add_obs(bus, &mpipe_jpeg_codec_subscriber, K_FOREVER));
	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_PLAYING));
	zassert_ok(zbus_sub_wait_msg(&mpipe_jpeg_codec_subscriber, &channel, &message,
				     K_SECONDS(2)),
		   "timed out waiting for EOS");
	zassert_equal(message.type, MPIPE_MESSAGE_EOS, "expected EOS, got %d (code %d)",
		      message.type, message.code);
	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_READY));
	zassert_ok(zbus_chan_rm_obs(bus, &mpipe_jpeg_codec_subscriber, K_FOREVER));

	zassert_ok(read_file(output_path, output, sizeof(output), &output_size));
	if (raw_first) {
		bool rgb565 = pixfmt == VIDEO_PIX_FMT_RGB565 || pixfmt == VIDEO_PIX_FMT_RGB565X;
		size_t expected = rgb565 ? TEST_RGB_SIZE : TEST_YUV_SIZE;

		zassert_equal(output_size, expected, "decoded size: %zu", output_size);
		if (rgb565) {
			check_rgb565(output, pixfmt);
		} else {
			check_luma(output);
			check_chroma(output, pixfmt);
		}
	} else {
		zassert_true(output_size >= 4U, "JPEG output too small: %zu", output_size);
		zassert_equal(output[0], 0xffU, "missing JPEG SOI marker");
		zassert_equal(output[1], 0xd8U, "missing JPEG SOI marker");
		zassert_equal(output[output_size - 2U], 0xffU, "missing JPEG EOI marker");
		zassert_equal(output[output_size - 1U], 0xd9U, "missing JPEG EOI marker");
		zassert_ok(get_jpeg_dimensions(output, output_size, &width, &height));
		zassert_equal(width, TEST_WIDTH, "JPEG width %u", width);
		zassert_equal(height, TEST_HEIGHT, "JPEG height %u", height);
		check_jpeg_content(output, output_size);
	}

	if (IS_ENABLED(CONFIG_MPIPE_JPEG_CODEC_FILE_KEEP_FILES)) {
		TC_PRINT("preserved %s/%s\n", TEST_FS_DIR, input_name);
		TC_PRINT("preserved %s/%s\n", TEST_FS_DIR, output_name);
	} else {
		(void)fs_unlink(input_path);
		(void)fs_unlink(output_path);
	}
}

ZTEST(mpipe_jpeg_codec_file, test_raw_rgb565_encode_decode)
{
	run_codec_pipeline(true, VIDEO_PIX_FMT_RGB565);
}

ZTEST(mpipe_jpeg_codec_file, test_raw_rgb565x_encode_decode)
{
	run_codec_pipeline(true, VIDEO_PIX_FMT_RGB565X);
}

ZTEST(mpipe_jpeg_codec_file, test_raw_yuv420_encode_decode)
{
	run_codec_pipeline(true, VIDEO_PIX_FMT_YUV420);
}

ZTEST(mpipe_jpeg_codec_file, test_raw_nv12_encode_decode)
{
	run_codec_pipeline(true, VIDEO_PIX_FMT_NV12);
}

ZTEST(mpipe_jpeg_codec_file, test_jpeg_rgb565_decode_encode)
{
	run_codec_pipeline(false, VIDEO_PIX_FMT_RGB565);
}

ZTEST(mpipe_jpeg_codec_file, test_jpeg_yuv420_decode_encode)
{
	run_codec_pipeline(false, VIDEO_PIX_FMT_YUV420);
}

ZTEST(mpipe_jpeg_codec_file, test_jpeg_nv12_decode_encode)
{
	run_codec_pipeline(false, VIDEO_PIX_FMT_NV12);
}

static void *mpipe_jpeg_codec_file_setup(void)
{
	zassert_ok(fs_mount(&test_mount), "failed to mount native test filesystem");
	return NULL;
}

static void mpipe_jpeg_codec_file_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_ok(fs_unmount(&test_mount), "failed to unmount native test filesystem");
}

ZTEST_SUITE(mpipe_jpeg_codec_file, NULL, mpipe_jpeg_codec_file_setup, NULL, NULL,
	    mpipe_jpeg_codec_file_teardown);
