#define write capture_write
#define timerfd_settime capture_timer
#define main idle_main
#include "watch_sensor_idle.c"
#undef main
#undef write
#undef timerfd_settime
#include <assert.h>

static unsigned commands, last_command, deadline_ms;
static int fail_timer, fail_write;
ssize_t capture_write(int fd, const void *data, size_t size) {
  (void)fd;
  const uint8_t *bytes = data;
  assert(size == 4 && bytes[0] == 0xb4 && bytes[1] == 0x0d && bytes[3] == 0);
  commands++;
  last_command = bytes[2];
  return fail_write ? -1 : (ssize_t)size;
}
int capture_timer(int fd, int flags, const struct itimerspec *timer,
                  struct itimerspec *old) {
  (void)fd;
  (void)flags;
  (void)old;
  deadline_ms =
      timer->it_value.tv_sec * 1000 + timer->it_value.tv_nsec / 1000000;
  return fail_timer ? -1 : 0;
}

// Policy line: version, stamp, screen off, samples, interval ms, HR running, OS alive, frozen count.
int main(void) {
  // Stale or future policy files must not trigger wrist rearming or transport recovery.
  char dir[] = "/tmp/pbl-fifo-policy-review.XXXXXX";
  assert(mkdtemp(dir));
  snprintf(s_dir, sizeof(s_dir), "%s", dir);
  char path[256];
  snprintf(path, sizeof(path), "%s/fifo-policy", dir);
  long long now = seconds(CLOCK_MONOTONIC);
  FILE *policy = fopen(path, "w");
  assert(policy);
  fprintf(policy, "2 %lld 0 125 40 0 1 3\n", now);
  fclose(policy);
  assert(!current_policy() && s_screen_off == 0 && !s_frozen_new);
  policy = fopen(path, "w");
  assert(policy);
  fprintf(policy, "2 %lld 1 125 40 0 1 4\n", now - 21);
  fclose(policy);
  assert(!current_policy() && s_screen_off == -1 && !s_frozen_new);
  policy = fopen(path, "w");
  assert(policy);
  fprintf(policy, "2 %lld 1 125 40 0 1 5\n", now + 60);
  fclose(policy);
  assert(!current_policy() && s_screen_off == -1 && !s_frozen_new);
  policy = fopen(path, "w");
  assert(policy);
  fprintf(policy, "2 %lld 1 125 40 0 1 4\n", now);
  fclose(policy);
  assert(current_policy() && s_screen_off == 1 && s_frozen_new);
  s_frozen_new = false;
  unlink(path);
  assert(!current_policy() && s_screen_off == -1);
  rmdir(dir);
  s_frozen_seen = ~0u;
  s_frozen_new = false;
  assert(can_batch("2 100 1 125 40 0 1 0", 100));
  assert(can_batch("2 100 1 125 40 0 1 0", 120));
  assert(s_buffer_ms == 4000);
  // Screen-off 5 Hz: 100 samples per 20 s flush; slower rates are capped at 20 s.
  assert(can_batch("2 100 1 125 200 0 1 0", 120) && s_buffer_ms == 20000);
  assert(can_batch("2 100 1 125 250 0 1 0", 120) && s_buffer_ms == 20000);
  assert(can_batch("2 100 1 125 100 0 1 0", 120) && s_buffer_ms == 10000);
  assert(!can_batch("2 100 1 125 251 0 1 0", 120));
  assert(can_batch("2 100 1 125 40 0 1 0", 120) && s_buffer_ms == 4000);
  const char *rejected[] = {
      "2 100 0 125 40 0 1 0",   // screen on
      "2 100 1 4 40 0 1 0",     // too few samples
      "2 100 1 125 20 0 1 0",   // interval below the hub's safe range
      "2 100 1 125 40 1 1 0",   // heart rate running
      "2 100 1 0 40 0 1 0",     // accelerometer stopped
      "2 100 1 125 40 0 0 0",   // no PebbleOS to consume the samples
      "1 100 1 125 40 0 1 0",   // the old version
      "3 100 1 125 40 0 1 0",   // a newer one
      "2 121 1 125 40 0 1 0",   // from the future
      "2 99 1 125 40 0 1 0",    // stale
      "2 100 1 125",            // short
      "invalid",
  };
  for (unsigned i = 0; i < sizeof(rejected) / sizeof(*rejected); i++)
    assert(!can_batch(rejected[i], 120));
  // A rising frozen count asks for one transport restart; the first reading only sets the base.
  note_frozen(3);
  assert(!s_frozen_new);
  note_frozen(3);
  assert(!s_frozen_new);
  note_frozen(4);
  assert(s_frozen_new);
  s_frozen_new = false;
  note_frozen(4);
  assert(!s_frozen_new);
  assert(update(false, true) == 0 && commands == 0);
  assert(update(true, false) == 0 && commands == 1 && last_command == 0x22);
  assert(s_idle.mode == BUFFERING && deadline_ms == BUFFER_MS);
  assert(update(true, false) == 0 && commands == 1);
  assert(update(true, true) == 0 && commands == 2 && last_command == 0x21);
  assert(s_idle.mode == FLUSHING && deadline_ms == FLUSH_MS);
  assert(update(true, false) == 0 && commands == 2);
  assert(update(true, true) == 0 && commands == 3 && last_command == 0x22);
  // Screen wake, HR or foreground sensor demand preempts a buffered cycle
  // immediately.
  assert(update(false, true) == 0 && commands == 4 && last_command == 0x21);
  assert(s_idle.mode == LIVE && deadline_ms == 0);
  assert(update(false, false) == 0 && commands == 4);
  fail_timer = 1;
  assert(update(true, false) == -1 && s_idle.mode == BUFFERING);
  fail_timer = 0;
  assert(update(false, false) == 0 && s_idle.mode == LIVE &&
         last_command == 0x21);
  fail_write = 1;
  assert(update(true, false) == -1 && s_idle.mode == LIVE);
  puts("SENSOR_IDLE_POLICY_FAIL_CLOSED_FIFO_DEADLINE_WAKE_HRM_APP_PREEMPTION_"
       "OK");
  return 0;
}
