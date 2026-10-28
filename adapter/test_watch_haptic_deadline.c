#include <assert.h>
#include <pbl/drivers/linux_adapter.h>
#include <stdio.h>

static struct pbl_adapter_vibe vibe(unsigned strength, unsigned duration_ms, uint64_t now_ms) {
  struct pbl_adapter_vibe message;
  pbl_adapter_build_vibe(&message, 1, strength, duration_ms);
  // Rebase the deadline so the cases do not depend on the real clock.
  if (message.deadline_ms)
    message.deadline_ms = now_ms + message.duration_ms;
  return message;
}

int main(void) {
  unsigned max = PBL_ADAPTER_VIBE_MAX_MS;
  struct pbl_adapter_vibe message = vibe(80, 60, 1000);
  assert(pbl_adapter_vibe_remaining_ms(&message, 1000, max) == 60);
  assert(pbl_adapter_vibe_remaining_ms(&message, 1037, max) == 23);
  assert(pbl_adapter_vibe_remaining_ms(&message, 1060, max) == 0);
  assert(pbl_adapter_vibe_remaining_ms(&message, 9000, max) == 0);
  assert(pbl_adapter_vibe_remaining_ms(&message, 999, max) == 0);
  message.strength = 101;
  assert(pbl_adapter_vibe_remaining_ms(&message, 1000, max) == 0);
  message = vibe(100, 0, 1000);
  assert(pbl_adapter_vibe_remaining_ms(&message, 1000, max) == 0);
  // Longer than the protocol allows is clamped when built, and refused when forged.
  message = vibe(100, 5001, 1000);
  assert(message.duration_ms == 5000);
  message.duration_ms = 5001;
  message.deadline_ms = 6001;
  assert(pbl_adapter_vibe_remaining_ms(&message, 1000, max) == 0);
  message = vibe(100, 5000, UINT64_C(1) << 40);
  assert(pbl_adapter_vibe_remaining_ms(&message, UINT64_C(1) << 40, max) == 5000);
  // The adapter's declared maximum applies.
  message = vibe(100, 800, 1000);
  assert(pbl_adapter_vibe_remaining_ms(&message, 1000, 500) == 0);
  // A stop has no strength and never vibrates.
  pbl_adapter_build_vibe(&message, 1, 0, 0);
  assert(message.strength == 0 && message.duration_ms == 0 && message.deadline_ms == 0);
  assert(pbl_adapter_vibe_remaining_ms(&message, 0, max) == 0);
  puts("HAPTIC_DEADLINES_OK: delayed, expired, invalid and long-uptime commands");
}
