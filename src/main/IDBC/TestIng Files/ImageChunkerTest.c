#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "ImageChunker.h"

/**
 * Test case for a 1000-byte image.
 * A 1000-byte image should produce:
 *
 * Chunk 0 -> 400 bytes
 *         -> is_last_chunk: false
 * Chunk 1 -> 400 bytes
 *         -> is_last_chunk: false
 * Chunk 2 -> 200 bytes
 *         -> is_last_chunk: true
 */
bool testNormalImage(void) {
    printf("Running testNormalImage...\n");

    // Create a fake 1000-byte image.
    uint8_t imageBytes[1000];

    for (size_t i = 0; i < sizeof(imageBytes); i++) {
        imageBytes[i] = (uint8_t)(i % 256);
    }

    // Describe our image using ImageData.
    ImageData image = {
        .data = imageBytes,
        .size = sizeof(imageBytes),
        .format = IDBC_ImageFormat_IMAGE_FORMAT_JPEG
    };

    // Check the expected number of chunks.
    size_t totalChunks = calculateTotalChunks(image.size);
    if (totalChunks != 3) {
        printf("FAIL: Expected 3 chunks, got %zu.\n", totalChunks);
        return false;
    }

    // Create a buffer where we will reconstruct the image.
    uint8_t reassembledImage[1000];
    size_t reassembledSize = 0;

    // Build every chunk and put its payload back into the reconstructed image.
    for (uint32_t sequence = 0; sequence < totalChunks; sequence++) {
        IDBC_ImageChunk chunk;

        uint32_t eventId = 123;
        uint32_t imageIndex = 0;

        bool success = buildImageChunk(
            &image,
            eventId,
            imageIndex,
            sequence,
            &chunk
        );

        if (!success) {
            printf("FAIL: Could not build chunk %u.\n", sequence);
            return false;
        }

        // Check the expected payload size.
        size_t expectedSize;

        if (sequence < 2) {
            expectedSize = 400;
        }
        else {
            expectedSize = 200;
        }

        if (chunk.payload.size != expectedSize) {
            printf("FAIL: Chunk %u expected %zu bytes, got %u.\n", sequence, expectedSize, (unsigned)chunk.payload.size);
            return false;
        }

        // Check the last-chunk flag.
        bool expectedLast = (sequence == 2);
        if (chunk.is_last_chunk != expectedLast) {
            printf("FAIL: Chunk %u has incorrect is_last_chunk value.\n", sequence);
            return false;
        }

        // Put the chunk back into its original position.
        size_t offset = (size_t)chunk.chunk_sequence * IMAGE_CHUNK_DATA_SIZE;
        memcpy(reassembledImage + offset, chunk.payload.bytes, chunk.payload.size);
        reassembledSize += chunk.payload.size;
    }

    // Make sure the reconstructed image has the expected size.
    if (reassembledSize != image.size) {
        printf("FAIL: Reassembled size is %zu, expected %zu.\n", reassembledSize, image.size);
        return false;
    }

    /*
     * Compare every byte of the reconstructed image against
     * the original.
     */
    if (memcmp(imageBytes, reassembledImage, image.size) != 0) {
        printf("FAIL: Reassembled image does not match original.\n");
        return false;
    }

    printf("PASS: testNormalImage\n\n");
    return true;
}

/**
 * Test case for a 400-byte image.
 * A 400-byte image should produce:
 * 
 * Chunk 0 -> 400 bytes
 *         -> is_last_chunk: true
 */
bool testExactChunkSize(void) {
    printf("Running testExactChunkSize...\n");

    // Create an image that is exactly one chunk in size.
    uint8_t imageBytes[IMAGE_CHUNK_DATA_SIZE];
    for (size_t i = 0; i < sizeof(imageBytes); i++) {
        imageBytes[i] = (uint8_t)(i % 256);
    }

    ImageData image = {
        .data = imageBytes,
        .size = sizeof(imageBytes),
        .format = IDBC_ImageFormat_IMAGE_FORMAT_JPEG
    };

    // Exactly 400 bytes should require exactly one chunk.
    size_t totalChunks = calculateTotalChunks(image.size);

    if (totalChunks != 1) {
        printf("FAIL: Expected 1 chunk, got %zu.\n", totalChunks);
        return false;
    }

    // Build the only chunk.
    IDBC_ImageChunk chunk;
    bool success = buildImageChunk(&image, 123, 0, 0, &chunk);
    if (!success) {
        printf("FAIL: Could not build chunk 0.\n");
        return false;
    }

    // The chunk should contain all 400 bytes.
    if (chunk.payload.size != IMAGE_CHUNK_DATA_SIZE) {
        printf("FAIL: Expected payload of %d bytes, got %u.\n",
            IMAGE_CHUNK_DATA_SIZE, (unsigned)chunk.payload.size);
        return false;
    }

    // Since this is the only chunk, it should be marked as the last chunk.
    if (!chunk.is_last_chunk) {
        printf("FAIL: Single chunk was not marked as last.\n");
        return false;
    }

    // Verify that the actual bytes are correct.
    if (memcmp(imageBytes, chunk.payload.bytes, IMAGE_CHUNK_DATA_SIZE) != 0) {
        printf("FAIL: Chunk payload does not match original image.\n");
        return false;
    }

    printf("PASS: testExactChunkSize\n\n");
    return true;
}

/**
 * Test case for a 401-byte image.
 * A 401-byte image should produce:
 * 
 * Chunk 0 -> 400 bytes
 *         -> is_last_chunk: false
 * Chunk 1 -> 1 byte
 *         -> is_last_chunk: true
 */
bool testJustOverChunkSize(void) {
    printf("Running testJustOverChunkSize...\n");

    // Create an image that is one byte larger than a single chunk.
    uint8_t imageBytes[IMAGE_CHUNK_DATA_SIZE + 1];
    for (size_t i = 0; i < sizeof(imageBytes); i++) {
        imageBytes[i] = (uint8_t)(i % 256);
    }

    ImageData image = {
        .data = imageBytes,
        .size = sizeof(imageBytes),
        .format = IDBC_ImageFormat_IMAGE_FORMAT_JPEG
    };

    // 401 bytes should require exactly 2 chunks.
    size_t totalChunks = calculateTotalChunks(image.size);
    if (totalChunks != 2) {
        printf("FAIL: Expected 2 chunks, got %zu.\n", totalChunks);
        return false;
    }

    // Create a buffer for the reconstructed image.
    uint8_t reassembledImage[IMAGE_CHUNK_DATA_SIZE + 1];
    size_t reassembledSize = 0;

    // Build both chunks.
    for (uint32_t sequence = 0; sequence < totalChunks; sequence++) {
        IDBC_ImageChunk chunk;

        bool success = buildImageChunk(
            &image,
            123,
            0,
            sequence,
            &chunk
        );

        if (!success) {
            printf("FAIL: Could not build chunk %u.\n", sequence);
            return false;
        }

        /*
         * Chunk 0 should contain 400 bytes.
         * Chunk 1 should contain the final 1 byte.
         */
        size_t expectedSize;

        if (sequence == 0) {
            expectedSize = IMAGE_CHUNK_DATA_SIZE;
        }
        else {
            expectedSize = 1;
        }

        if (chunk.payload.size != expectedSize) {
            printf("FAIL: Chunk %u expected %zu bytes, got %u.\n", sequence, expectedSize, (unsigned)chunk.payload.size);
            return false;
        }

        // Only chunk 1 should be marked as the last chunk.
        bool expectedLast = (sequence == 1);

        if (chunk.is_last_chunk != expectedLast) {
            printf("FAIL: Chunk %u has incorrect is_last_chunk value.\n", sequence);
            return false;
        }

        // Put this chunk back into its original position.
        size_t offset = (size_t)chunk.chunk_sequence * IMAGE_CHUNK_DATA_SIZE;
        memcpy(reassembledImage + offset, chunk.payload.bytes, chunk.payload.size);

        reassembledSize += chunk.payload.size;
    }

    // The reconstructed image should contain all 401 bytes.
    if (reassembledSize != image.size) {
        printf("FAIL: Reassembled size is %zu, expected %zu.\n", reassembledSize, image.size);
        return false;
    }

    // Verify that every byte matches the original image.
    if (memcmp(imageBytes, reassembledImage, image.size) != 0) {
        printf("FAIL: Reassembled image does not match original.\n");
        return false;
    }

    printf("PASS: testJustOverChunkSize\n\n");
    return true;
}


int main(void) {
    bool allPassed = true;

    // Run our tests.
    allPassed &= testNormalImage();
    allPassed &= testExactChunkSize();
    allPassed &= testJustOverChunkSize();

    // Report the overall result.
    if (allPassed) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }

    printf("SOME TESTS FAILED.\n");
    return 1;
}