#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ring_buffer.h"

enum {
  BENCHMARK_OPS = 250000,
  BENCHMARK_SLOTS = 2048,
  BENCHMARK_PAYLOAD_SIZE = 192,
};

static double elapsed_seconds(struct timespec start, struct timespec end) {
  return (double)(end.tv_sec - start.tv_sec) +
         (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

int main(void) {
  char payload[BENCHMARK_PAYLOAD_SIZE];
  memset(payload, 'N', sizeof(payload));

  struct ring_buffer *rb = NULL;
  ring_buffer_init(&rb, BENCHMARK_SLOTS);
  if (rb == NULL) {
    fprintf(stderr, "ring_buffer_init failed\n");
    return 1;
  }

  struct timespec start;
  struct timespec end;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    fprintf(stderr, "clock_gettime start failed\n");
    ring_buffer_free(&rb);
    return 1;
  }

  for (int i = 0; i < BENCHMARK_OPS; i++) {
    payload[0] = (char)('A' + (i % 26));
    ring_buffer_push_back(rb, payload, sizeof(payload));
  }

  if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
    fprintf(stderr, "clock_gettime end failed\n");
    ring_buffer_free(&rb);
    return 1;
  }

  const double seconds = elapsed_seconds(start, end);
  const double ops_per_second = seconds > 0.0
      ? (double)BENCHMARK_OPS / seconds
      : 0.0;

  printf("{\n");
  printf("  \"benchmark\": \"ring_buffer_push_back\",\n");
  printf("  \"operations\": %d,\n", BENCHMARK_OPS);
  printf("  \"slots\": %d,\n", BENCHMARK_SLOTS);
  printf("  \"payload_bytes\": %d,\n", BENCHMARK_PAYLOAD_SIZE);
  printf("  \"seconds\": %.9f,\n", seconds);
  printf("  \"ops_per_second\": %.2f\n", ops_per_second);
  printf("}\n");

  ring_buffer_free(&rb);
  return 0;
}
