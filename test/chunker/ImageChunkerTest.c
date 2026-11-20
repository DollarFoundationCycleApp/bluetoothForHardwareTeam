#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ImageChunker.h"

void setUp(void) {}
void tearDown(void) {}

void test_normal_image_1000_bytes(void) {
    uint8_t imageBytes[1000];
    for (size_t i = 0; i < sizeof(imageBytes); i++) {
        imageBytes[i] = (uint8_t)(i % 256);
    }

    ImageData image = {
        .data = imageBytes,
        .size = sizeof(imageBytes),
        .format = IDBC_ImageFormat_IMAGE_FORMAT_JPEG
    };

    size_t totalChunks = calculateTotalChunks(image.size);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)totalChunks);

    uint8_t reassembledImage[1000];
    size_t reassembledSize = 0;

    for (uint32_t sequence = 0; sequence < totalChunks; sequence++) {
        IDBC_ImageChunk chunk;
        TEST_ASSERT_TRUE(buildImageChunk(&image, 123, 0, sequence, &chunk));

        uint32_t expectedSize = (sequence < 2) ? 400 : 200;
        TEST_ASSERT_EQUAL_UINT32(expectedSize, chunk.payload.size);
        TEST_ASSERT_EQUAL(sequence == 2, chunk.is_last_chunk);

        size_t offset = (size_t)chunk.chunk_sequence * IMAGE_CHUNK_DATA_SIZE;
        memcpy(reassembledImage + offset, chunk.payload.bytes, chunk.payload.size);
        reassembledSize += chunk.payload.size;
    }

    TEST_ASSERT_EQUAL_UINT32((uint32_t)image.size, (uint32_t)reassembledSize);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(imageBytes, reassembledImage, image.size);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_normal_image_1000_bytes);
    return UNITY_END();
}