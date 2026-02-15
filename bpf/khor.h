#pragma once

#include <stdint.h>

// Keep this struct small and fixed-size. No strings in the MVP.
enum khor_event_type : uint32_t {
  KHOR_EV_EXEC = 1,
  KHOR_EV_NET_RX = 2,
  KHOR_EV_NET_TX = 3,
};

struct khor_event {
  uint64_t ts_ns;
  uint32_t pid;
  uint32_t tgid;
  uint32_t type;
  uint32_t _pad;
  uint64_t a; // type-specific payload (e.g., bytes)
  uint64_t b; // type-specific payload (e.g., ifindex)
};

