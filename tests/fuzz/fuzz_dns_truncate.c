#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "dns_truncate.h"
#include "logging.h"

static void init_logging_once(void) {
  static int initialized = 0;
  if (initialized) {
    return;
  }
  initialized = 1;

  int fd = open("/dev/null", O_WRONLY);
  if (fd >= 0) {
    logging_init(fd, LOG_FATAL, 0);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  init_logging_once();

  if (size < 4) {
    return 0;
  }

  const size_t req_len = ((size_t)data[0] << 8U) | data[1];
  const size_t available = size - 2U;
  if (req_len == 0 || req_len >= available) {
    return 0;
  }

  const char *req = (const char *)&data[2];
  const size_t resp_len_in = available - req_len;
  if (resp_len_in == 0 || resp_len_in > 65535U) {
    return 0;
  }

  char resp[65535];
  for (size_t i = 0; i < resp_len_in; i++) {
    resp[i] = (char)data[2U + req_len + i];
  }

  size_t resp_len = resp_len_in;
  dns_truncate_for_udp(req, req_len, resp, &resp_len);
  return 0;
}
