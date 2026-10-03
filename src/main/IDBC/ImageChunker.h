#ifndef IMAGE_CHUNKER_H
#define IMAGE_CHUNKER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "IBDC_v0.3.2.pb.h"

/*
 * Maximum number of image bytes carried by one ImageChunk.
 *
 * This is the project's chosen Bluetooth payload size.
 */
#define IMAGE_CHUNK_DATA_SIZE 400

/*
 * Represents an image that has already been loaded into memory.
 *
 * data   = pointer to the image's raw bytes
 * size   = number of bytes in the image
 * format = JPEG or PNG
 */
typedef struct {
    const uint8_t* data;
    size_t size;
    IDBC_ImageFormat format;
} ImageData;

/*
 * Determines whether the supplied bytes contain a supported
 * image format.
 *
 * Returns true if JPEG or PNG is detected.
 * The detected format is written to *format.
 */
bool detectImageFormat(
    const uint8_t* data,
    size_t size,
    IDBC_ImageFormat* format
);

/*
 * Calculates how many 400-byte chunks are required
 * to transmit an image.
 */
size_t calculateTotalChunks(size_t imageSize);

/*
 * Builds one ImageChunk from an ImageData object.
 *
 * chunkSequence identifies which 400-byte section to build.
 *
 * Returns true if the chunk was successfully created.
 */
bool buildImageChunk(
    const ImageData* image,
    uint32_t eventId,
    uint32_t imageIndex,
    uint32_t chunkSequence,
    IDBC_ImageChunk* output
);

#endif