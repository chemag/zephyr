/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/video.h>
#include <zephyr/fs/fs.h>
#include <zephyr/mpipe/fs/mpipe_file_sink.h>
#include <zephyr/mpipe/fs/mpipe_file_src.h>
#include <zephyr/mpipe/img/mpipe_img_jpeg_decoder.h>
#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_message.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "test_jpeg_17x17.h"

#define TEST_MOUNT_POINT "/test_fs"
#define TEST_INPUT_PATH  TEST_MOUNT_POINT "/input.jpg"
#define TEST_OUTPUT_PATH TEST_MOUNT_POINT "/output.rgb565"
#define TEST_WIDTH       17U
#define TEST_HEIGHT      17U
#define TEST_OUTPUT_SIZE (TEST_WIDTH * TEST_HEIGHT * sizeof(uint16_t))

ZBUS_MSG_SUBSCRIBER_DEFINE(mpipe_jpeg_decoder_subscriber);

enum {
	PIPE_ID,
	FILE_SRC_ID,
	JPEG_DECODER_ID,
	FILE_SINK_ID,
};

static struct fs_mount_t test_mount = {
	.type = FS_NATIVE_MOUNT,
	.mnt_point = TEST_MOUNT_POINT,
	.fs_data = TEST_FS_DIR,
};

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

static uint16_t get_pixel(const uint8_t *data, uint32_t x, uint32_t y)
{
	size_t offset = ((size_t)y * TEST_WIDTH + x) * sizeof(uint16_t);

	return (uint16_t)data[offset] | ((uint16_t)data[offset + 1U] << 8);
}

static void check_pixel(const uint8_t *data, uint32_t x, uint32_t y, uint8_t min_r,
			uint8_t min_g, uint8_t min_b, uint8_t max_r, uint8_t max_g, uint8_t max_b)
{
	uint16_t pixel = get_pixel(data, x, y);
	uint8_t r = (pixel >> 11) & 0x1fU;
	uint8_t g = (pixel >> 5) & 0x3fU;
	uint8_t b = pixel & 0x1fU;

	zassert_between_inclusive(r, min_r, max_r, "red at %u,%u: %u", x, y, r);
	zassert_between_inclusive(g, min_g, max_g, "green at %u,%u: %u", x, y, g);
	zassert_between_inclusive(b, min_b, max_b, "blue at %u,%u: %u", x, y, b);
}

ZTEST(mpipe_jpeg_decoder_file, test_jpeg_file_to_raw_file)
{
	static struct mpipe pipeline;
	static struct mpipe_file_src file_src;
	static struct mpipe_img_jpeg_decoder jpeg_decoder;
	static struct mpipe_file_sink file_sink;
	static uint8_t output[TEST_OUTPUT_SIZE];
	struct mpipe_structure caps;
	const struct zbus_channel *channel;
	struct zbus_channel *bus;
	struct mpipe_message message;
	uint32_t block_size = sizeof(jpeg_17x17);
	size_t output_size = 0U;

	(void)fs_unlink(TEST_INPUT_PATH);
	(void)fs_unlink(TEST_OUTPUT_PATH);
	zassert_ok(write_file(TEST_INPUT_PATH, jpeg_17x17, sizeof(jpeg_17x17)),
		   "failed to create JPEG input file");

	memset(&pipeline, 0, sizeof(pipeline));
	memset(&file_src, 0, sizeof(file_src));
	memset(&jpeg_decoder, 0, sizeof(jpeg_decoder));
	memset(&file_sink, 0, sizeof(file_sink));

	zassert_ok(mpipe_pipeline_init(&pipeline, PIPE_ID));
	zassert_ok(mpipe_file_src_init(&file_src, FILE_SRC_ID));
	zassert_ok(mpipe_img_jpeg_decoder_init(&jpeg_decoder, JPEG_DECODER_ID));
	zassert_ok(mpipe_file_sink_init(&file_sink, FILE_SINK_ID));
	zassert_ok(mpipe_structure_init_fields(
		&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT,
		VIDEO_PIX_FMT_JPEG, MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT, TEST_WIDTH,
		MPIPE_CAPS_IMAGE_HEIGHT, MPIPE_TYPE_UINT, TEST_HEIGHT, MPIPE_CAPS_END));
	zassert_ok(mpipe_pad_set_caps(&file_src.src.src_pad, &caps));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&file_src,
					       MPIPE_PROP_FS_SRC_PATH, TEST_INPUT_PATH,
					       MPIPE_PROP_FS_SRC_BLOCKSIZE, &block_size,
					       MPIPE_PROP_LIST_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&file_sink,
					       MPIPE_PROP_FS_SINK_PATH, TEST_OUTPUT_PATH,
					       MPIPE_PROP_LIST_END));
	zassert_ok(mpipe_bin_add((struct mpipe_bin *)&pipeline, (struct mpipe_element *)&file_src,
				 (struct mpipe_element *)&jpeg_decoder,
				 (struct mpipe_element *)&file_sink, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)&file_src,
				      (struct mpipe_element *)&jpeg_decoder,
				      (struct mpipe_element *)&file_sink, NULL));

	bus = mpipe_element_get_bus_chan((struct mpipe_element *)&pipeline);
	zassert_ok(zbus_chan_add_obs(bus, &mpipe_jpeg_decoder_subscriber, K_FOREVER));
	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_PLAYING));
	zassert_ok(zbus_sub_wait_msg(&mpipe_jpeg_decoder_subscriber, &channel, &message,
				     K_SECONDS(2)),
		   "timed out waiting for EOS");
	zassert_equal(message.type, MPIPE_MESSAGE_EOS, "expected EOS, got %d (code %d)",
		      message.type, message.code);
	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_READY));
	zassert_ok(zbus_chan_rm_obs(bus, &mpipe_jpeg_decoder_subscriber, K_FOREVER));

	zassert_ok(read_file(TEST_OUTPUT_PATH, output, sizeof(output), &output_size),
		   "failed to read decoded file");
	zassert_equal(output_size, sizeof(output), "decoded size: %zu", output_size);
	check_pixel(output, 3U, 3U, 20U, 0U, 0U, 31U, 20U, 12U);
	check_pixel(output, 13U, 3U, 0U, 35U, 0U, 15U, 63U, 15U);
	check_pixel(output, 3U, 13U, 0U, 0U, 20U, 15U, 20U, 31U);
	check_pixel(output, 13U, 13U, 22U, 45U, 22U, 31U, 63U, 31U);
	check_pixel(output, 8U, 8U, 0U, 0U, 0U, 12U, 24U, 12U);

	if (IS_ENABLED(CONFIG_MPIPE_JPEG_DECODER_FILE_KEEP_FILES)) {
		TC_PRINT("preserved %s and %s\n", TEST_INPUT_PATH, TEST_OUTPUT_PATH);
	} else {
		(void)fs_unlink(TEST_INPUT_PATH);
		(void)fs_unlink(TEST_OUTPUT_PATH);
	}
}

static void *mpipe_jpeg_decoder_file_setup(void)
{
	zassert_ok(fs_mount(&test_mount), "failed to mount native test filesystem");
	return NULL;
}

static void mpipe_jpeg_decoder_file_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_ok(fs_unmount(&test_mount), "failed to unmount native test filesystem");
}

ZTEST_SUITE(mpipe_jpeg_decoder_file, NULL, mpipe_jpeg_decoder_file_setup, NULL, NULL,
	    mpipe_jpeg_decoder_file_teardown);
