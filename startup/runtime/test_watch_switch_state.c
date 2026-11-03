/* Exercise the listener's global-key-state fallback without physical devices.
 */
#define main watch_switch_main
#include "watch_os_switch.c"
#undef main
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "CHECK_FAILED line=%d\n", __LINE__);                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static void set_key(unsigned char *keys, unsigned code, bool down) {
  if (down)
    keys[code / 8] |= 1u << (code % 8);
  else
    keys[code / 8] &= ~(1u << (code % 8));
}
int main(void) {
  s_count = 2;
  unsigned char power[KEY_STATE_BYTES] = {0}, back[KEY_STATE_BYTES] = {0};
  set_key(power, KEY_POWER, true);
  apply_key_state(&s_inputs[0], power);
  CHECK(s_physical_buttons == (1u << LINUX_BUTTON_SELECT));
  CHECK(!s_debug_chord_active);
  set_key(back, KEY_BACK, true);
  apply_key_state(&s_inputs[1], back);
  CHECK(s_debug_chord_active);
  uint64_t since = s_debug_chord_since;
  for (int i = 0; i < 100; i++) {
    apply_key_state(&s_inputs[0], power);
    apply_key_state(&s_inputs[1], back);
  }
  CHECK(s_debug_chord_since == since);
  set_key(power, KEY_POWER, false);
  set_key(power, KEY_HOME, true);
  apply_key_state(&s_inputs[0], power);
  CHECK(s_debug_chord_active && s_debug_chord_since == since);
  set_key(back, KEY_BACK, false);
  apply_key_state(&s_inputs[1], back);
  CHECK(!s_debug_chord_active);
  memset(power, 0, sizeof(power));
  apply_key_state(&s_inputs[0], power);
  CHECK(!s_physical_buttons);
  /* A failed query produces an all-released snapshot, cancelling a stale hold.
   */
  set_key(power, KEY_POWER, true);
  set_key(power, KEY_BACK, true);
  apply_key_state(&s_inputs[0], power);
  CHECK(s_debug_chord_active);
  memset(power, 0, sizeof(power));
  apply_key_state(&s_inputs[0], power);
  CHECK(!s_debug_chord_active && !s_physical_buttons);
  puts("SWITCH_KEY_STATE_CHORD_REPEAT_HOME_RELEASE_FAILURE_OK");
  return 0;
}
