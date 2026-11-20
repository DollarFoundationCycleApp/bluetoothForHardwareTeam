#include <unity.h>

extern "C" {
  void test_image_chunk_protobuf_roundtrip(void);
  void test_normal_image_1000_bytes(void);
}

void test_fifo_device_storage_limit(void);
void test_duplicate_device_deduplication(void);

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_image_chunk_protobuf_roundtrip);
  RUN_TEST(test_normal_image_1000_bytes);
  RUN_TEST(test_fifo_device_storage_limit);
  RUN_TEST(test_duplicate_device_deduplication);
  return UNITY_END();
}