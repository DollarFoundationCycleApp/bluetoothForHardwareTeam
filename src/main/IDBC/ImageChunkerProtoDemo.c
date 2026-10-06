#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "IBDC_v0.3.2.pb.h"
#include "pb_encode.h"
#include "pb_decode.h"

int main(void) {
    printf("========================================\n");
    printf("      ImageChunk Protobuf Demo\n");
    printf("========================================\n\n");

    // Step 1: Create and ImageChunk
    IDBC_ImageChunk original = IDBC_ImageChunk_init_default;

    original.event_id = 123;
    original.image_index = 0;
    original.chunk_sequence = false;
    original.total_chunks = 98;

    // Populate payload with predictable test data.
    original.payload.size = 400;

    for (size_t i = 0; i < 400; i++) {
        original.payload.bytes[i] = (uint8_t)(i % 256);
    }

    printf("Original ImageChunk:\n");
    printf("  event_id:      %u\n", (unsigned)original.event_id);
    printf("  image_index:   %u\n", (unsigned)original.image_index);
    printf("  chunk_sequence: %u\n", (unsigned)original.chunk_sequence);
    printf("  is_last_chunk: %s\n", original.is_last_chunk ? "true" : "false");
    printf("  total_chunks:  %u\n", (unsigned)original.total_chunks);
    printf("  payload size:  %u bytes\n\n", (unsigned)original.payload.size);

    // Step 2: Encode the ImageChunk
    uint8_t encoded[IDBC_ImageChunk_size];

    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));

    bool status = pb_encode(&stream, IDBC_ImageChunk_fields, &original);

    if (!status) {
        printf("ERROR: Protobuf encoding failed:\n  %s\n", PB_GET_ERROR(&stream));
        return 1;
    }

    printf("Protobuf encoding:\n");
    printf("  Buffer capacity: %zu bytes\n", sizeof(encoded));
    printf("  Encoded size:    %zu bytes\n\n", stream.bytes_written);

    // Step 3: Decode the protobuf bytes
    IDBC_ImageChunk decoded = IDBC_ImageChunk_init_default;

    pb_istream_t input = pb_istream_from_buffer(encoded, stream.bytes_written);
    status = pb_decode(&input, IDBC_ImageChunk_fields, &decoded);

    if (!status) {
        printf("ERROR: Protobuf decoding failed:\n  %s\n", PB_GET_ERROR(&input));
        return 1;
    }

    printf("Decoded ImageChunk:\n");
    printf("  event_id:      %u\n", (unsigned)decoded.event_id);
    printf("  image_index:   %u\n", (unsigned)decoded.image_index);
    printf("  chunk_sequence: %u\n", (unsigned)decoded.chunk_sequence);
    printf("  is_last_chunk: %s\n", decoded.is_last_chunk ? "true" : "false");
    printf("  total_chunks:  %u\n", (unsigned)decoded.total_chunks);
    printf("  payload size:  %u bytes\n\n", (unsigned)decoded.payload.size);

    // Step 4: Verify the decoded message
    printf("Verifying decoded data...\n");

    if (decoded.event_id != original.event_id ||
        decoded.image_index != original.image_index ||
        decoded.chunk_sequence != original.chunk_sequence ||
        decoded.is_last_chunk != original.is_last_chunk ||
        decoded.total_chunks != original.total_chunks) {
        printf("FAIL: Metadata does not match.\n");
        return 1;
    }

    if (decoded.payload.size != original.payload.size) {
        printf("FAIL: Payload sizes do not match.\n");
        return 1;
    }

    if (memcmp(decoded.payload.bytes, original.payload.bytes, original.payload.size) != 0) {
        printf("FAIL: Payload data does not match.\n");
        return 1;
    }

    printf("PASS: Metadata matches.\n");
    printf("PASS: Payload matches.\n\n");

    printf("========================================\n");
    printf("             DEMO PASSED\n");
    printf("========================================\n");

    return 0;
}