/* Runtime-only MCU FIFO policy for the investigated SM-R850 sensor stack. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <pbl/drivers/linux_adapter.h>

// Runtime directory shared with the adapter, which writes the policy this helper follows.
#define GALAXY_RUNTIME_DIR "/opt/usr/home/owner/share/pebble"
static char s_dir[128];
#define BUFFER_MS 4000
// Each flush moves this many samples; 250 made the hub MCU reset.
#define FLUSH_SAMPLES 100
#define MAX_BUFFER_MS 20000
// Let the kernel finish resuming the hub link before talking to the hub.
#define RESUME_SETTLE_US 200000
#define FLUSH_MS 350
enum idle_mode { LIVE, BUFFERING, FLUSHING };
static struct {
  int hub, alarm;
  enum idle_mode mode;
} s_idle = {.hub = -1, .alarm = -1};
static volatile sig_atomic_t s_stopped;
// Adapter screen state from the last policy read: -1 unknown, 0 interactive, 1 off.
static int s_screen_off = -1;
// Buffering period for the adapter's measured sample interval.
static unsigned s_buffer_ms = BUFFER_MS;

static int64_t seconds(clockid_t clock) {
  struct timespec now;
  clock_gettime(clock, &now);
  return now.tv_sec;
}

// Policy line, written by the adapter: version, monotonic seconds, screen off, accel samples,
// accel interval ms, HR running, OS alive, count of frozen-accelerometer events.
static bool parse_policy(const char *line, long long *stamp, unsigned *ambient, unsigned *samples,
                         unsigned *interval, unsigned *hrm, unsigned *alive, unsigned *frozen) {
  unsigned version;
  return sscanf(line, "%u %lld %u %u %u %u %u %u", &version, stamp, ambient, samples, interval,
                hrm, alive, frozen) == 8 && version == 2;
}

static bool can_batch(const char *line, int64_t now) {
  unsigned ambient, samples, interval, hrm, alive, frozen;
  long long stamp;
  if (!parse_policy(line, &stamp, &ambient, &samples, &interval, &hrm, &alive, &frozen))
    return false;
  // Buffering is only safe while PebbleOS is there to consume the samples later.
  if (stamp < 0 || stamp > now || now - stamp > 20 || !alive || ambient != 1 ||
      samples != 125 || interval < 40 || interval > 250 || hrm != 0)
    return false;
  // The adapter slows the accelerometer while the screen is off.
  s_buffer_ms = interval * FLUSH_SAMPLES;
  if (s_buffer_ms > MAX_BUFFER_MS)
    s_buffer_ms = MAX_BUFFER_MS;
  return true;
}

// The adapter counts frozen-accelerometer events; a rise asks for a sensor transport restart.
static unsigned s_frozen_seen = ~0u;
static bool s_frozen_new;
static void note_frozen(unsigned frozen) {
  if (s_frozen_seen != ~0u && frozen != s_frozen_seen)
    s_frozen_new = true;
  s_frozen_seen = frozen;
}

static bool current_policy(void) {
  s_screen_off = -1;
  char line[256], path[300];
  snprintf(path, sizeof(path), "%s/fifo-policy", s_dir);
  FILE *file = fopen(path, "r");
  if (!file)
    return false;
  bool read = fgets(line, sizeof(line), file) != NULL;
  fclose(file);
  if (!read)
    return false;
  unsigned ambient, samples, interval, hrm, alive, frozen;
  long long stamp;
  int64_t now = seconds(CLOCK_MONOTONIC);
  if (!parse_policy(line, &stamp, &ambient, &samples, &interval, &hrm, &alive, &frozen) ||
      stamp < 0 || stamp > now || now - stamp > 20)
    return false;
  s_screen_off = ambient == 1;
  note_frozen(frozen);
  return can_batch(line, now);
}

static int hub_state(bool sleep) {
  // Installed HAL: Sleep -> 0x22, Wakeup -> 0x21; kernel validates this
  // envelope.
  const uint8_t message[] = {0xb4, 0x0d, sleep ? 0x22 : 0x21, 0};
  return write(s_idle.hub, message, sizeof(message)) == sizeof(message) ? 0
                                                                        : -1;
}

static int arm(unsigned milliseconds) {
  struct itimerspec timer = {
      .it_value = {.tv_sec = milliseconds / 1000,
                   .tv_nsec = (milliseconds % 1000) * 1000000}};
  return timerfd_settime(s_idle.alarm, 0, &timer, NULL);
}

static int update(bool eligible, bool expired) {
  if (!eligible) {
    if (s_idle.mode != LIVE) {
      int rc = hub_state(false);
      s_idle.mode = LIVE;
      arm(0);
      printf("SENSOR_IDLE live t=%lld\n", (long long)seconds(CLOCK_BOOTTIME));
      return rc;
    }
    return 0;
  }
  if (s_idle.mode == LIVE || (s_idle.mode == FLUSHING && expired)) {
    // 100 new samples per cycle. 250-sample (10 s) batches made the hub MCU reset on flush,
    // which leaves its accelerometer frozen (no raise-to-wake, no steps).
    if (hub_state(true))
      return -1;
    s_idle.mode = BUFFERING;
    static unsigned logged_ms;
    if (logged_ms != s_buffer_ms) {
      logged_ms = s_buffer_ms;
      printf("SENSOR_IDLE cycle_ms=%u t=%lld\n", s_buffer_ms, (long long)seconds(CLOCK_BOOTTIME));
    }
    return arm(s_buffer_ms);
  }
  if (s_idle.mode == BUFFERING && expired) {
    // The alarm often fires mid-resume; a command before the kernel's own resume reset the hub.
    usleep(RESUME_SETTLE_US);
    if (hub_state(false))
      return -1;
    s_idle.mode = FLUSHING;
    return arm(FLUSH_MS);
  }
  return 0;
}

// A sensor-hub MCU reset drops the wrist-up gesture configuration, and sensord only re-sends
// it when the algorithm changes. Re-send the current one when the screen turns off.
#define WRISTUP_REARM_MIN_S 60
static void rearm_wristup(int screen_off_before) {
  static int64_t last = -WRISTUP_REARM_MIN_S;
  int64_t now = seconds(CLOCK_BOOTTIME);
  if (screen_off_before != 0 || s_screen_off != 1 || now - last < WRISTUP_REARM_MIN_S)
    return;
  last = now;
  pid_t child = fork();
  if (child == 0) {
    char script[300];
    snprintf(script, sizeof(script), "%s/watch-wristup-rearm", s_dir);
    execl("/bin/sh", "sh", script, (char *)NULL);
    _exit(127);
  }
}

// The adapter flags a wedged accelerometer; restarting Samsung's sensor transport (lhd) restores
// live samples. The hub can re-freeze shortly after, so allow a restart once per minute.
#define LHD_RESTART_MIN_S 60
static void recover_frozen_accel(void) {
  static int64_t last = -LHD_RESTART_MIN_S;
  if (!s_frozen_new)
    return;
  s_frozen_new = false;
  int64_t now = seconds(CLOCK_BOOTTIME);
  if (now - last < LHD_RESTART_MIN_S)
    return;
  last = now;
  printf("SENSOR_IDLE accel frozen, restarting lhd t=%lld\n", (long long)now);
  pid_t child = fork();
  if (child == 0) {
    execl("/bin/systemctl", "systemctl", "restart", "lhd.service", (char *)NULL);
    _exit(127);
  }
}

static void stop(int signal) {
  (void)signal;
  s_stopped = 1;
}

#include "watch_session.h"

int main(int argc, char **argv) {
  if (argc != 2)
    return 64;
  if (!strcmp(argv[1], "--session-modes")) {
    puts(WATCH_SESSION_MODES);
    return 0;
  }
  long duration;
  if (!watch_session_parse(argv[1], &duration))
    return 64;
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (pbl_adapter_runtime_dir(s_dir, sizeof(s_dir), NULL, GALAXY_RUNTIME_DIR))
    return 64;
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  signal(SIGCHLD, SIG_IGN);
  int notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (notify < 0)
    return 1;
  if (inotify_add_watch(notify, s_dir,
                        IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE) < 0) {
    close(notify);
    return 2;
  }
  // An alarm, rather than an ordinary timer, prevents a sleeping AP overflowing
  // the FIFO.
  s_idle.alarm =
      timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
  s_idle.hub = open("/dev/ssp_sensorhub", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (s_idle.alarm < 0 || s_idle.hub < 0) {
    perror("sensor idle setup");
    if (s_idle.alarm >= 0)
      close(s_idle.alarm);
    if (s_idle.hub >= 0)
      close(s_idle.hub);
    close(notify);
    return 3;
  }
  int result = 0;
  int64_t deadline = seconds(CLOCK_BOOTTIME) + duration;
  struct pollfd fds[] = {{.fd = notify, .events = POLLIN},
                         {.fd = s_idle.alarm, .events = POLLIN}};
  printf("SENSOR_IDLE_READY duration=%ld interval_ms=%d flush_ms=%d\n", duration, BUFFER_MS,
         FLUSH_MS);
  while (!s_stopped && watch_session_active(duration, seconds(CLOCK_BOOTTIME), deadline)) {
    if (update(current_policy(), false)) {
      result = 4;
      break;
    }
    int n = poll(fds, 2, 5000);
    if (n < 0 && errno != EINTR) {
      result = 5;
      break;
    }
    // Only on a timeout: then at least RESUME_CHECK_MS have passed since the thaw, so the
    // kernel has finished resuming the hub link. Our own alarm wake flushes instead.
    if (n <= 0)
      continue;
    if (fds[0].revents & POLLIN) {
      char events[8192];
      while (read(notify, events, sizeof(events)) > 0) {
      }
    }
    bool expired = false;
    if (fds[1].revents & POLLIN) {
      uint64_t count;
      expired = read(s_idle.alarm, &count, sizeof(count)) == sizeof(count);
    }
    int screen_off_before = s_screen_off;
    if (update(current_policy(), expired)) {
      result = 6;
      break;
    }
    rearm_wristup(screen_off_before);
    recover_frozen_accel();
  }
  if (s_idle.mode != LIVE)
    hub_state(false);
  close(s_idle.hub);
  close(s_idle.alarm);
  close(notify);
  printf("SENSOR_IDLE_STOPPED result=%d\n", result);
  return result;
}
