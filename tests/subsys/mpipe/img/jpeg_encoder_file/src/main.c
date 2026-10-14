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
#include <zephyr/mpipe/img/mpipe_img_jpeg_encoder.h>
#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_message.h>
#include <zephyr/mpipe/mpipe_structure.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#define TEST_MOUNT_POINT "/test_fs"
#define TEST_INPUT_PATH  TEST_MOUNT_POINT "/input.rgb565"
#define TEST_OUTPUT_PATH TEST_MOUNT_POINT "/output.jpg"
#define TEST_WIDTH       17U
#define TEST_HEIGHT      17U
#define TEST_INPUT_SIZE  (TEST_WIDTH * TEST_HEIGHT * sizeof(uint16_t))
#define TEST_OUTPUT_SIZE 4096U

ZBUS_MSG_SUBSCRIBER_DEFINE(mpipe_jpeg_encoder_subscriber);

enum {
	PIPE_ID,
	FILE_SRC_ID,
	JPEG_ENCODER_ID,
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

ZTEST(mpipe_jpeg_encoder_file, test_raw_file_to_jpeg_file)
{
	static struct mpipe pipeline;
	static struct mpipe_file_src file_src;
	static struct mpipe_img_jpeg_encoder jpeg_encoder;
	static struct mpipe_file_sink file_sink;
	static uint16_t input[TEST_WIDTH * TEST_HEIGHT];
	static uint8_t output[TEST_OUTPUT_SIZE];
	struct mpipe_structure caps;
	const struct zbus_channel *channel;
	struct zbus_channel *bus;
	struct mpipe_message message;
	uint32_t block_size = TEST_INPUT_SIZE;
	uint16_t jpeg_width = 0U;
	uint16_t jpeg_height = 0U;
	size_t output_size = 0U;

	for (uint32_t y = 0U; y < TEST_HEIGHT; y++) {
		for (uint32_t x = 0U; x < TEST_WIDTH; x++) {
			uint16_t pixel;

			if (x == TEST_WIDTH / 2U || y == TEST_HEIGHT / 2U) {
				pixel = 0x0000U;
			} else if (x < TEST_WIDTH / 2U && y < TEST_HEIGHT / 2U) {
				pixel = 0xf800U;
			} else if (x >= TEST_WIDTH / 2U && y < TEST_HEIGHT / 2U) {
				pixel = 0x07e0U;
			} else if (x < TEST_WIDTH / 2U) {
				pixel = 0x001fU;
			} else {
				pixel = 0xffffU;
			}

			input[y * TEST_WIDTH + x] = pixel;
		}
	}

	(void)fs_unlink(TEST_INPUT_PATH);
	(void)fs_unlink(TEST_OUTPUT_PATH);
	zassert_ok(write_file(TEST_INPUT_PATH, (const uint8_t *)input, sizeof(input)),
		   "failed to create input file");

	memset(&pipeline, 0, sizeof(pipeline));
	memset(&file_src, 0, sizeof(file_src));
	memset(&jpeg_encoder, 0, sizeof(jpeg_encoder));
	memset(&file_sink, 0, sizeof(file_sink));

	zassert_ok(mpipe_pipeline_init(&pipeline, PIPE_ID));
	zassert_ok(mpipe_file_src_init(&file_src, FILE_SRC_ID));
	zassert_ok(mpipe_img_jpeg_encoder_init(&jpeg_encoder, JPEG_ENCODER_ID));
	zassert_ok(mpipe_file_sink_init(&file_sink, FILE_SINK_ID));

	zassert_ok(mpipe_structure_init_fields(
		&caps, MPIPE_MEDIA_VIDEO, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_TYPE_UINT,
		VIDEO_PIX_FMT_RGB565, MPIPE_CAPS_IMAGE_WIDTH, MPIPE_TYPE_UINT, TEST_WIDTH,
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
				 (struct mpipe_element *)&jpeg_encoder,
				 (struct mpipe_element *)&file_sink, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)&file_src,
				      (struct mpipe_element *)&jpeg_encoder,
				      (struct mpipe_element *)&file_sink, NULL));

	bus = mpipe_element_get_bus_chan((struct mpipe_element *)&pipeline);
	zassert_ok(zbus_chan_add_obs(bus, &mpipe_jpeg_encoder_subscriber, K_FOREVER));

	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_PLAYING));
	zassert_ok(zbus_sub_wait_msg(&mpipe_jpeg_encoder_subscriber, &channel, &message,
				     K_SECONDS(2)),
		   "timed out waiting for EOS");
	zassert_equal(message.type, MPIPE_MESSAGE_EOS, "expected EOS, got %d (code %d)",
		      message.type, message.code);
	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_READY));

	zassert_ok(zbus_chan_rm_obs(bus, &mpipe_jpeg_encoder_subscriber, K_FOREVER));
	zassert_ok(read_file(TEST_OUTPUT_PATH, output, sizeof(output), &output_size),
		   "failed to read output file");
	zassert_true(output_size >= 4U, "JPEG output too small: %zu", output_size);
	zassert_equal(output[0], 0xffU, "missing JPEG SOI marker");
	zassert_equal(output[1], 0xd8U, "missing JPEG SOI marker");
	zassert_equal(output[output_size - 2U], 0xffU, "missing JPEG EOI marker");
	zassert_equal(output[output_size - 1U], 0xd9U, "missing JPEG EOI marker");
	zassert_ok(get_jpeg_dimensions(output, output_size, &jpeg_width, &jpeg_height),
		   "failed to find JPEG dimensions");
	zassert_equal(jpeg_width, TEST_WIDTH, "JPEG width %u", jpeg_width);
	zassert_equal(jpeg_height, TEST_HEIGHT, "JPEG height %u", jpeg_height);

	if (IS_ENABLED(CONFIG_MPIPE_JPEG_ENCODER_FILE_KEEP_FILES)) {
		TC_PRINT("preserved %s and %s\n", TEST_INPUT_PATH, TEST_OUTPUT_PATH);
	} else {
		(void)fs_unlink(TEST_INPUT_PATH);
		(void)fs_unlink(TEST_OUTPUT_PATH);
	}
}

static void *mpipe_jpeg_encoder_file_setup(void)
{
	zassert_ok(fs_mount(&test_mount), "failed to mount native test filesystem");

	return NULL;
}

static void mpipe_jpeg_encoder_file_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_ok(fs_unmount(&test_mount), "failed to unmount native test filesystem");
}

ZTEST_SUITE(mpipe_jpeg_encoder_file, NULL, mpipe_jpeg_encoder_file_setup, NULL, NULL,
	    mpipe_jpeg_encoder_file_teardown);
