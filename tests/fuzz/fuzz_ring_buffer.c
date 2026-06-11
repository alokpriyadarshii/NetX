#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ring_buffer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0) {
    return 0;
  }

  struct ring_buffer *rb = NULL;
  const uint32_t slots = (uint32_t)(data[0] % 32U);
  ring_buffer_init(&rb, slots);
  if (rb == NULL) {
    return 0;
  }

  size_t pos = 1;
  while (pos < size) {
    const uint32_t chunk_len = (uint32_t)(data[pos] % 64U);
    pos++;
    const size_t remaining = size - pos;
    const uint32_t write_len = chunk_len < remaining
        ? chunk_len
        : (uint32_t)remaining;
    ring_buffer_push_back(rb, (char *)&data[pos], write_len);
    pos += write_len;
  }

  FILE *sink = tmpfile();
  if (sink != NULL) {
    ring_buffer_dump(rb, sink);
    fclose(sink);
  }

  ring_buffer_free(&rb);
  return 0;
}
