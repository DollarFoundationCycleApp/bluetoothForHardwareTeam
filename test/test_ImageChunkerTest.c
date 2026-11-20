#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ImageChunker.h"

void test_normal_image_chunking(void) {

    enum { TEST_IMAGE_SIZE = 1000 };
    // start by figuring out bytes for a 1000-byte image
    uint8_t imageBytes[TEST_IMAGE_SIZE];
    for (size_t i = 0; i < sizeof(imageBytes); i++) {
        imageBytes[i] = (uint8_t)(i % 256);
    }

    ImageData image = {
        .data = imageBytes,
        .size = sizeof(imageBytes),
        .format = IDBC_ImageFormat_IMAGE_FORMAT_JPEG
    };

    // Calculate expected number of chunks
    size_t expectedTotalChunks =
        (image.size + IMAGE_CHUNK_DATA_SIZE - 1) / IMAGE_CHUNK_DATA_SIZE;
    // run through the function
    size_t totalChunks = calculateTotalChunks(image.size);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)expectedTotalChunks, (uint32_t)totalChunks);

    // test assembly option
    uint8_t reassembledImage[TEST_IMAGE_SIZE];
    size_t reassembledSize = 0;

    // go through each chunk and build it and then add on so it'd seem like we're reassembling.
    for (uint32_t sequence = 0; sequence < totalChunks; sequence++) {
        IDBC_ImageChunk chunk;
        TEST_ASSERT_TRUE(buildImageChunk(&image, 123, 0, sequence, &chunk));

        size_t offset = (size_t)sequence * IMAGE_CHUNK_DATA_SIZE;
        size_t remaining = image.size - offset;
        size_t expectedSize =
            (remaining < IMAGE_CHUNK_DATA_SIZE) ? remaining : IMAGE_CHUNK_DATA_SIZE;
        bool expectedLast = (sequence == totalChunks - 1);

        // were we so far successful?
        TEST_ASSERT_EQUAL_UINT32((uint32_t)expectedSize, chunk.payload.size);
        TEST_ASSERT_EQUAL(expectedLast, chunk.is_last_chunk);

        memcpy(reassembledImage + offset, chunk.payload.bytes, chunk.payload.size);
        reassembledSize += chunk.payload.size;
    }
    // we truly made it so did we miss something?
    TEST_ASSERT_EQUAL_UINT32((uint32_t)image.size, (uint32_t)reassembledSize);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(imageBytes, reassembledImage, image.size);
}