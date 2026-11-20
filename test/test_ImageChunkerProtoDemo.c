#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "IBDC_v0.3.2.pb.h"
#include "../src/Nanopb/pb_encode.h"
#include "../src/Nanopb/pb_decode.h"

void test_image_chunk_protobuf_roundtrip(void) {
    IDBC_ImageChunk original = IDBC_ImageChunk_init_default;
    original.event_id = 123;
    original.image_index = 0;
    original.chunk_sequence = 0;
    original.total_chunks = 98;
    original.payload.size = 400;

    for (size_t i = 0; i < 400; i++) {
        original.payload.bytes[i] = (uint8_t)(i % 256);
    }

    uint8_t encoded[IDBC_ImageChunk_size];
    pb_ostream_t out_stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    TEST_ASSERT_TRUE(pb_encode(&out_stream, IDBC_ImageChunk_fields, &original));

    IDBC_ImageChunk decoded = IDBC_ImageChunk_init_default;
    pb_istream_t in_stream = pb_istream_from_buffer(encoded, out_stream.bytes_written);
    TEST_ASSERT_TRUE(pb_decode(&in_stream, IDBC_ImageChunk_fields, &decoded));

    TEST_ASSERT_EQUAL_UINT32(original.event_id, decoded.event_id);
    TEST_ASSERT_EQUAL_UINT32(original.image_index, decoded.image_index);
    TEST_ASSERT_EQUAL_UINT32(original.chunk_sequence, decoded.chunk_sequence);
    TEST_ASSERT_EQUAL(original.is_last_chunk, decoded.is_last_chunk);
    TEST_ASSERT_EQUAL_UINT32(original.total_chunks, decoded.total_chunks);
    TEST_ASSERT_EQUAL_UINT32(original.payload.size, decoded.payload.size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original.payload.bytes, decoded.payload.bytes, original.payload.size);
}
