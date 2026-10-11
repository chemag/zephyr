/*
 * Copyright 2026 Meta Platforms, Inc. and affiliates.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/mpipe/fs/mpipe_file_sink.h>
#include <zephyr/mpipe/fs/mpipe_file_src.h>
#include <zephyr/mpipe/mpipe.h>
#include <zephyr/mpipe/mpipe_message.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#define TEST_MOUNT_POINT "/test_fs"
#define TEST_INPUT_PATH  TEST_MOUNT_POINT "/input.raw"
#define TEST_OUTPUT_PATH TEST_MOUNT_POINT "/output.raw"
#define TEST_DATA_SIZE   257U
#define TEST_BLOCK_SIZE  17U

ZBUS_MSG_SUBSCRIBER_DEFINE(mpipe_fs_subscriber);

enum {
	PIPE_ID,
	FILE_SRC_ID,
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
	size_t offset = 0;
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
	size_t offset = 0;
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

ZTEST(mpipe_fs, test_file_src_to_file_sink_round_trip)
{
	static struct mpipe pipeline;
	static struct mpipe_file_src file_src;
	static struct mpipe_file_sink file_sink;
	static uint8_t input[TEST_DATA_SIZE];
	static uint8_t output[TEST_DATA_SIZE + 1U];
	const struct zbus_channel *channel;
	struct zbus_channel *bus;
	struct mpipe_message message;
	uint32_t block_size = TEST_BLOCK_SIZE;
	size_t output_size = 0;

	for (size_t i = 0; i < sizeof(input); i++) {
		input[i] = (uint8_t)((i * 37U + 11U) & UINT8_MAX);
	}

	(void)fs_unlink(TEST_INPUT_PATH);
	(void)fs_unlink(TEST_OUTPUT_PATH);
	zassert_ok(write_file(TEST_INPUT_PATH, input, sizeof(input)),
		   "failed to create input file");

	memset(&pipeline, 0, sizeof(pipeline));
	memset(&file_src, 0, sizeof(file_src));
	memset(&file_sink, 0, sizeof(file_sink));

	zassert_ok(mpipe_pipeline_init(&pipeline, PIPE_ID));
	zassert_ok(mpipe_file_src_init(&file_src, FILE_SRC_ID));
	zassert_ok(mpipe_file_sink_init(&file_sink, FILE_SINK_ID));

	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&file_src,
					       MPIPE_PROP_FS_SRC_PATH, TEST_INPUT_PATH,
					       MPIPE_PROP_FS_SRC_BLOCKSIZE, &block_size,
					       MPIPE_PROP_LIST_END));
	zassert_ok(mpipe_object_set_properties((struct mpipe_object *)&file_sink,
					       MPIPE_PROP_FS_SINK_PATH, TEST_OUTPUT_PATH,
					       MPIPE_PROP_LIST_END));

	zassert_ok(mpipe_bin_add((struct mpipe_bin *)&pipeline,
				 (struct mpipe_element *)&file_src,
				 (struct mpipe_element *)&file_sink, NULL));
	zassert_ok(mpipe_element_link((struct mpipe_element *)&file_src,
				      (struct mpipe_element *)&file_sink, NULL));

	bus = mpipe_element_get_bus_chan((struct mpipe_element *)&pipeline);
	zassert_ok(zbus_chan_add_obs(bus, &mpipe_fs_subscriber, K_FOREVER));

	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_PLAYING));
	zassert_ok(zbus_sub_wait_msg(&mpipe_fs_subscriber, &channel, &message, K_SECONDS(2)),
		   "timed out waiting for EOS");
	zassert_equal(message.type, MPIPE_MESSAGE_EOS, "expected EOS, got %d (code %d)",
		      message.type, message.code);
	zassert_ok(mpipe_element_set_state((struct mpipe_element *)&pipeline, MPIPE_STATE_READY));

	zassert_ok(zbus_chan_rm_obs(bus, &mpipe_fs_subscriber, K_FOREVER));
	zassert_ok(read_file(TEST_OUTPUT_PATH, output, sizeof(output), &output_size),
		   "failed to read output file");
	zassert_equal(output_size, sizeof(input), "output size %zu, expected %zu", output_size,
		      sizeof(input));
	zassert_mem_equal(output, input, sizeof(input), "output differs from input");

	(void)fs_unlink(TEST_INPUT_PATH);
	(void)fs_unlink(TEST_OUTPUT_PATH);
}

static void *mpipe_fs_setup(void)
{
	zassert_ok(fs_mount(&test_mount), "failed to mount native test filesystem");

	return NULL;
}

static void mpipe_fs_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	zassert_ok(fs_unmount(&test_mount), "failed to unmount native test filesystem");
}

ZTEST_SUITE(mpipe_fs, NULL, mpipe_fs_setup, NULL, NULL, mpipe_fs_teardown);
