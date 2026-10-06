#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "ImageChunker.h"

// Reads an entire binary file into memory, returns true on success.
bool readFile(
    const char* filename,
    uint8_t** data,
    size_t* size
)
{
    FILE* file = fopen(filename, "rb");
    if (file == NULL) {return false;}

    // Find the size of the file.
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    long fileSize = ftell(file);

    if (fileSize < 0) {
        fclose(file);
        return false;
    }

    rewind(file);

    // Allocate enough memory for the entire image.
    uint8_t* buffer = malloc((size_t)fileSize);
    if (buffer == NULL) {
        fclose(file);
        return false;
    }

    // Read the image bytes into buffer.
    size_t bytesRead = fread(buffer, 1, (size_t)fileSize, file);

    fclose(file);

    if (bytesRead != (size_t)fileSize) {
        free(buffer);
        return false;
    }

    *data = buffer;
    *size = bytesRead;

    return true;
}

int main(int argc, char* argv[]) {
    // The program expects: ImageChunkerDemo.exe input.jpg output.jpg
    if (argc != 3) {
        printf("Usage: %s <input.jpg> <output.jpg>\n", argv[0]);
        return 1;
    }

    const char* inputFilename = argv[1];
    const char* outputFilename = argv[2];

    printf("========================================\n");
    printf("       Image Chunker JPEG Demo\n");
    printf("========================================\n\n");

    // Step 1: Read the image
    printf("Reading image: %s\n", inputFilename);

    uint8_t* imageBytes = NULL;
    size_t imageSize = 0;

    if (!readFile(inputFilename, &imageBytes, &imageSize)) {
        printf("ERROR: Could not read image file.\n");
        return 1;
    }

    printf("Image size: %zu bytes\n\n", imageSize);

    // Step 2: Detect the image format
    IDBC_ImageFormat format;

    if (!detectImageFormat(imageBytes, imageSize, &format)) {
        printf("ERROR: Image is not a supported JPEG or PNG.\n");
        free(imageBytes);
        return 1;
    }

    if (format == IDBC_ImageFormat_IMAGE_FORMAT_JPEG) {
        printf("Format: JPEG\n\n");
    }
    else if (format == IDBC_ImageFormat_IMAGE_FORMAT_PNG) {
        printf("Format: PNG\n\n");
    }
    else {
        printf("ERROR: Unknown image format.\n");
        free(imageBytes);
        return 1;
    }

    // Wrap the raw image bytes in ImageData.
    ImageData image = {
        .data = imageBytes,
        .size = imageSize,
        .format = format
    };

    // Step 3: Calculate the number of chunks
    size_t totalChunks = calculateTotalChunks(image.size);

    printf("Chunk size: %d bytes\n", IMAGE_CHUNK_DATA_SIZE);

    printf("Total chunks: %zu\n\n", totalChunks);

    // Step 4: Build chunks and reassemble them
    /*
     * We only need one ImageChunk at a time.
     * This is closer to how the real hardware will
     * eventually send the image over Bluetooth.
     */
    printf("Building chunks and reassembling...\n");

    uint8_t* reassembledImage = malloc(image.size);

    if (reassembledImage == NULL) {
        printf("ERROR: Could not allocate reassembly buffer.\n");
        free(imageBytes);
        return 1;
    }

    size_t reassembledSize = 0;

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
            printf("ERROR: Could not build chunk %u.\n", sequence);

            free(reassembledImage);
            free(imageBytes);
            return 1;
        }

        // Put this chunk back into its original position in the image.
        size_t offset = (size_t)chunk.chunk_sequence * IMAGE_CHUNK_DATA_SIZE;

        memcpy(reassembledImage + offset, chunk.payload.bytes, chunk.payload.size);
        reassembledSize += chunk.payload.size;

        // Only print a few chunks so a large image doesn't produce hundreds of lines.
        if (sequence < 3 || sequence == totalChunks - 1) {
            printf(
                "  Chunk %u: %u bytes%s\n",
                sequence,
                (unsigned)chunk.payload.size,
                chunk.is_last_chunk
                    ? " (last)"
                    : ""
            );
        }
        else if (sequence == 3) {
            printf("  ...\n");
        }
    }

    printf("\n");

    // Step 5: Verify the reassembled image
    printf("Reassembled size: %zu bytes\n", reassembledSize);

    if (reassembledSize != image.size) {
        printf("FAIL: Reassembled size does not match original.\n");

        free(reassembledImage);
        free(imageBytes);
        return 1;
    }

    printf("Comparing original and reassembled image...\n");

    if (memcmp(imageBytes, reassembledImage, image.size) != 0) {
        printf("FAIL: Reassembled image does not match original.\n");

        free(reassembledImage);
        free(imageBytes);
        return 1;
    }

    printf("PASS: Files are byte-for-byte identical.\n\n");

    // Step 6: Write the reconstructed JPEG
    printf("Writing reconstructed image: %s\n", outputFilename);

    FILE* outputFile = fopen(outputFilename, "wb");

    if (outputFile == NULL) {
        printf("ERROR: Could not create output file.\n");

        free(reassembledImage);
        free(imageBytes);
        return 1;
    }

    size_t bytesWritten = fwrite(reassembledImage, 1, reassembledSize, outputFile);

    fclose(outputFile);

    if (bytesWritten != reassembledSize) {
        printf("ERROR: Could not write complete output file.\n");

        free(reassembledImage);
        free(imageBytes);
        return 1;
    }

    printf("Output successfully written.\n\n");

    // Clean up allocated memory.
    free(reassembledImage);
    free(imageBytes);

    printf("========================================\n");
    printf("              DEMO PASSED\n");
    printf("========================================\n");

    return 0;
}