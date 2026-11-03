/* Runtime-only Samsung -> Pebble listener. Pebble handles the reverse chord. */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#define SWITCH_MARKER "/opt/usr/home/owner/share/pebble/pebble-ui-active"
#ifndef PEBBLE_INPUT_SOURCE
#define PEBBLE_INPUT_SOURCE "../pebbleos/drivers/button/linux/input_linux_bottom.c"
#endif
#include PEBBLE_INPUT_SOURCE
#include "watch_session.h"
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/wait.h>
#define LAUNCHER                                                               \
  "/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024/"                  \
  "start_phone_pairing_review.sh"
// Samsung owns display policy while this listener observes the buttons.
bool display_linux_is_interactive(void) { return true; }
unsigned linux_adapter_display_actual(void) { return PBL_ADAPTER_DISPLAY_ON; }
void linux_watch_user_wake(void) {}
int linux_adapter_wake(unsigned reason, unsigned source) {
  (void)reason;
  (void)source;
  return 0;
}
int linux_adapter_display_idle(unsigned reason) {
  (void)reason;
  return 0;
}
static volatile sig_atomic_t stopped;
static pid_t launcher;
static pid_t panel_recovery;
static bool stock_wake_requested;
static uint64_t wake_check_at;
static uint64_t last_recovery;
#define KEY_STATE_BYTES ((KEY_MAX + 8) / 8)
static bool key_down(const unsigned char *keys, unsigned code) {
  return (keys[code / 8] & (1u << (code % 8))) != 0;
}
static void apply_key_state(struct watch_input *input,
                            const unsigned char *keys) {
  unsigned previous = input->buttons;
  prv_physical_button(input, LINUX_BUTTON_SELECT,
                      key_down(keys, KEY_POWER) || key_down(keys, KEY_HOME));
  prv_physical_button(input, LINUX_BUTTON_BACK, key_down(keys, KEY_BACK));
  if (input->buttons != previous)
    printf("OS_SWITCH_KEY_STATE fd=%d buttons=%u\n", input->fd, input->buttons);
}
static void sample_button_state(void) {
  for (int i = 0; i < s_count; i++) {
    struct watch_input *input = &s_inputs[i];
    if (input->touch || input->bezel)
      continue;
    unsigned char keys[KEY_STATE_BYTES] = {0};
    /* Global state remains readable when Samsung exclusively receives events.
     */
    if (ioctl(input->fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
      memset(keys, 0, sizeof(keys));
    apply_key_state(input, keys);
  }
}
static void stop(int sig) {
  (void)sig;
  stopped = 1;
}
uint64_t posix_host_monotonic_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void posix_host_hook_register(const struct posix_host_hook *hook) {
  (void)hook;
}
void linux_watch_button_changed(int button, bool pressed) {
  (void)button;
  if (pressed && (button == LINUX_BUTTON_BACK || button == LINUX_BUTTON_SELECT))
    stock_wake_requested = true;
}
void linux_watch_touch_changed(bool pressed, int x, int y) {
  (void)pressed;
  (void)x;
  (void)y;
}
void posix_host_quit(void) {
  if (launcher > 0)
    return;
  fflush(NULL);
  launcher = fork();
  if (!launcher) {
    int log = open("/opt/usr/home/owner/share/pebble/os-switch-launch.log",
                   O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log >= 0) {
      dup2(log, STDOUT_FILENO);
      dup2(log, STDERR_FILENO);
      close(log);
    }
    execl("/bin/sh", "sh", LAUNCHER, (char *)NULL);
    _exit(127);
  }
  printf("OS_SWITCH_LAUNCH pid=%ld\n", (long)launcher);
}
static bool pebble_active(void) {
  FILE *f = fopen(SWITCH_MARKER, "r");
  if (!f)
    return false;
  long pid = 0;
  int count = fscanf(f, "%ld", &pid);
  fclose(f);
  if (count != 1 || pid <= 1 || kill((pid_t)pid, 0))
    return false;
  char path[80], exe[256] = {0};
  snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
  ssize_t n = readlink(path, exe, sizeof(exe) - 1);
  return n > 0 && !strncmp(exe, "/opt/usr/home/owner/share/pebble/pebbleos",
                           strlen("/opt/usr/home/owner/share/pebble/pebbleos"));
}
static void check_stock_wake(uint64_t now) {
  if (!getenv("PBL_PANEL_RECOVERY"))
    return;
  if (stock_wake_requested) {
    stock_wake_requested = false;
    wake_check_at = now + 500000;
  }
  if (!wake_check_at || now < wake_check_at)
    return;
  wake_check_at = 0;
  if (panel_recovery > 0 || (last_recovery && now - last_recovery < 10000000))
    return;
  FILE *f = fopen("/sys/class/lcd/s6e36w4x01/lcd_power", "r");
  int power = -1;
  if (f) {
    if (fscanf(f, "%d", &power) != 1)
      power = -1;
    fclose(f);
  }
  if (power <= 0)
    return;
  last_recovery = now;
  panel_recovery = fork();
  if (!panel_recovery) {
    int log = open("/opt/usr/home/owner/share/pebble/os-switch-panel.log",
                   O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log >= 0) {
      dup2(log, 1);
      dup2(log, 2);
      close(log);
    }
    execl("/opt/usr/home/owner/share/tmp/rea-runtime-probe-20261024/"
          "watch-panel-recover",
          "watch-panel-recover", (char *)NULL);
    _exit(127);
  }
  printf("OS_SWITCH_PANEL_RECOVERY pid=%ld\n", (long)panel_recovery);
}
static void clear_input(void) {
  wake_check_at = 0;
  stock_wake_requested = false;
  s_physical_buttons = 0;
  s_debug_chord_active = false;
  s_debug_requested = false;
  s_bezel_pending = 0;
  s_bezel_button = -1;
  for (int i = 0; i < s_count; i++) {
    s_inputs[i].buttons = 0;
    struct input_event discard[32];
    while (read(s_inputs[i].fd, discard, sizeof(discard)) > 0) {
    }
  }
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc == 2 && !strcmp(argv[1], "--session-modes")) {
    puts(WATCH_SESSION_MODES);
    return 0;
  }
  long seconds = 3600;
  if (argc > 2 || (argc == 2 && !watch_session_parse(argv[1], &seconds)))
    return 64;
  unsetenv("PBL_INPUT_GRAB");
  unsetenv("PBL_OS_SWITCH");
  signal(SIGTERM, stop);
  signal(SIGINT, stop);
  prv_init();
  if (!s_count)
    return 2;
  uint64_t until = posix_host_monotonic_us() + (uint64_t)seconds * 1000000;
  bool previous = pebble_active();
  printf("OS_SWITCH_READY pebble_active=%d duration=%ld\n", previous, seconds);
  while (!stopped && watch_session_active(seconds, posix_host_monotonic_us(), until)) {
    bool active = pebble_active();
    if (active != previous) {
      clear_input();
      previous = active;
      printf("OS_SWITCH_INTERFACE=%s\n", active ? "pebble" : "samsung");
    }
    if (launcher > 0) {
      int status;
      if (waitpid(launcher, &status, WNOHANG) == launcher) {
        printf("OS_SWITCH_LAUNCH_FINISHED status=%d\n", status);
        launcher = 0;
      }
    }
    if (!active && launcher <= 0 && s_physical_buttons == 0)
      s_debug_requested = false;
    if (panel_recovery > 0) {
      int status;
      if (waitpid(panel_recovery, &status, WNOHANG) == panel_recovery) {
        printf("OS_SWITCH_PANEL_RECOVERY_FINISHED status=%d\n", status);
        panel_recovery = 0;
      }
    }
    struct pollfd fds[MAX_INPUTS];
    for (int i = 0; i < s_count; i++)
      fds[i] = (struct pollfd){.fd = s_inputs[i].fd, .events = POLLIN};
    /* Firmware owns input grabs while active; avoid polling readable stale
     * data. */
    poll(fds, active ? 0 : s_count, active ? 1000 : 50);
    if (!active) {
      sample_button_state();
      prv_poll();
      check_stock_wake(posix_host_monotonic_us());
    }
  }
  prv_exit();
  puts("OS_SWITCH_STOPPED");
  return 0;
}
