#include "ImageChunker.h"

#include <string.h>
#include <stdint.h>


bool detectImageFormat(
    const uint8_t* data,
    size_t size,
    IDBC_ImageFormat* format
) {
    if (data == NULL || format == NULL) {
        return false;
    }

    /*
     * JPEG files begin with FF D8.
     */
    if (size >= 2 &&
        data[0] == 0xFF &&
        data[1] == 0xD8) {

        *format = IDBC_ImageFormat_IMAGE_FORMAT_JPEG;
        return true;
    }

    /*
     * PNG files begin with:
     *
     * 89 50 4E 47 0D 0A 1A 0A
     */
    static const uint8_t pngSignature[] = {
        0x89, 0x50, 0x4E, 0x47,
        0x0D, 0x0A, 0x1A, 0x0A
    };

    if (size >= sizeof(pngSignature) &&
        memcmp(data, pngSignature, sizeof(pngSignature)) == 0) {

        *format = IDBC_ImageFormat_IMAGE_FORMAT_PNG;
        return true;
    }

    return false;
}


size_t calculateTotalChunks(size_t imageSize) {
    if (imageSize == 0) {
        return 0;
    }

    return (imageSize + IMAGE_CHUNK_DATA_SIZE - 1)
           / IMAGE_CHUNK_DATA_SIZE;
}


bool buildImageChunk(
    const ImageData* image,
    uint32_t eventId,
    uint32_t imageIndex,
    uint32_t chunkSequence,
    IDBC_ImageChunk* output
) {
    if (image == NULL ||
        image->data == NULL ||
        output == NULL ||
        image->size == 0) {

        return false;
    }

    size_t totalChunks = calculateTotalChunks(image->size);

    /*
     * Make sure the requested chunk actually exists.
     */
    if (chunkSequence >= totalChunks) {
        return false;
    }

    /*
     * Find the first byte belonging to this chunk.
     *
     * Chunk 0 -> byte 0
     * Chunk 1 -> byte 400
     * Chunk 2 -> byte 800
     * ...
     */
    size_t offset =
        (size_t)chunkSequence * IMAGE_CHUNK_DATA_SIZE;

    /*
     * Determine how many bytes remain in the image.
     */
    size_t remaining = image->size - offset;

    /*
     * Normally we copy 400 bytes.
     *
     * The final chunk may contain fewer than 400 bytes.
     */
    size_t bytesToCopy = remaining;

    if (bytesToCopy > IMAGE_CHUNK_DATA_SIZE) {
        bytesToCopy = IMAGE_CHUNK_DATA_SIZE;
    }

    /*
     * Start with an empty protobuf message.
     */
    *output = (IDBC_ImageChunk)IDBC_ImageChunk_init_default;

    /*
     * Fill in the ImageChunk metadata.
     */
    output->event_id = eventId;
    output->image_index = imageIndex;
    output->chunk_sequence = chunkSequence;
    output->total_chunks = (uint32_t)totalChunks;

    /*
     * True only for the final chunk.
     */
    output->is_last_chunk =
        (chunkSequence == totalChunks - 1);

    /*
     * Copy the actual image bytes into the protobuf payload.
     */
    output->payload.size = (pb_size_t)bytesToCopy;

    memcpy(
        output->payload.bytes,
        image->data + offset,
        bytesToCopy
    );

    return true;
}