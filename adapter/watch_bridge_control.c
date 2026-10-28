/* Diagnostic sender for the adapter protocol. Sends one message to a running adapter.
 *
 *   watch_bridge_control vibe STRENGTH MS
 *   watch_bridge_control backlight LEVEL
 *   watch_bridge_control display on|off|clock [timeout|user|palm|gesture|app]
 *   watch_bridge_control activity button|bezel|touch
 *   watch_bridge_control accel SAMPLES INTERVAL_US
 *   watch_bridge_control hrm 0|1
 *   watch_bridge_control idle 0|1 [DEADLINE_US]
 *   watch_bridge_control trace 0|1 [SECONDS]
 *
 * The adapter's replies go to the OS socket, so run this with PebbleOS stopped or expect the OS
 * to see them. The runtime directory comes from PBL_RUNTIME_DIR, XDG_RUNTIME_DIR, or the
 * Galaxy Watch3 default. */
#include <pbl/drivers/linux_adapter.h>

#define GALAXY_RUNTIME_DIR "/opt/usr/home/owner/share/pebble"

static int usage(void) {
  fputs("usage: watch_bridge_control vibe|backlight|display|activity|accel|hrm|idle|trace ...\n",
        stderr);
  return 2;
}

static long number(const char *text, long low, long high, int *bad) {
  char *end;
  long value = strtol(text, &end, 10);
  if (end == text || *end || value < low || value > high)
    *bad = 1;
  return value;
}

static int choose(const char *text, const char *const *names, int count, int first) {
  for (int i = 0; i < count; i++)
    if (!strcmp(text, names[i]))
      return first + i;
  return -1;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return usage();
  char dir[128];
  struct sockaddr_un peer;
  if (pbl_adapter_runtime_dir(dir, sizeof(dir), NULL, GALAXY_RUNTIME_DIR) ||
      pbl_adapter_socket_address(&peer, dir, PBL_ADAPTER_ADAPTER_SOCK))
    return 2;
  union {
    struct pbl_adapter_vibe vibe;
    struct pbl_adapter_backlight backlight;
    struct pbl_adapter_display display;
    struct pbl_adapter_user_activity activity;
    struct pbl_adapter_accel_config accel;
    struct pbl_adapter_hrm_config hrm;
    struct pbl_adapter_idle idle;
    struct pbl_adapter_trace trace;
  } m;
  size_t size = 0;
  int bad = 0;
  const char *command = argv[1];
  uint32_t seq = (uint32_t)getpid();
  if (!strcmp(command, "vibe") && argc == 4) {
    pbl_adapter_build_vibe(&m.vibe, seq, (unsigned)number(argv[2], 0, 100, &bad),
                           (unsigned)number(argv[3], 0, PBL_ADAPTER_VIBE_MAX_MS, &bad));
    size = sizeof(m.vibe);
  } else if (!strcmp(command, "backlight") && argc == 3) {
    pbl_adapter_init(&m, PBL_ADAPTER_BACKLIGHT, sizeof(m.backlight), seq);
    m.backlight.level = (uint8_t)number(argv[2], 0, 100, &bad);
    size = sizeof(m.backlight);
  } else if (!strcmp(command, "display") && (argc == 3 || argc == 4)) {
    static const char *const modes[] = {"on", "off", "clock"};
    static const char *const reasons[] = {"timeout", "user", "palm", "gesture", "app"};
    int mode = choose(argv[2], modes, 3, PBL_ADAPTER_DISPLAY_ON);
    int reason = argc == 4 ? choose(argv[3], reasons, 5, PBL_ADAPTER_REASON_TIMEOUT)
                           : PBL_ADAPTER_REASON_APP;
    if (mode < 0 || reason < 0)
      return usage();
    pbl_adapter_init(&m, PBL_ADAPTER_DISPLAY, sizeof(m.display), seq);
    m.display.mode = (uint8_t)mode;
    m.display.reason = (uint8_t)reason;
    size = sizeof(m.display);
  } else if (!strcmp(command, "activity") && argc == 3) {
    static const char *const sources[] = {"button", "bezel", "touch"};
    int source = choose(argv[2], sources, 3, PBL_ADAPTER_ACTIVITY_BUTTON);
    if (source < 0)
      return usage();
    pbl_adapter_init(&m, PBL_ADAPTER_USER_ACTIVITY, sizeof(m.activity), seq);
    m.activity.source = (uint8_t)source;
    size = sizeof(m.activity);
  } else if (!strcmp(command, "accel") && argc == 4) {
    pbl_adapter_init(&m, PBL_ADAPTER_ACCEL_CONFIG, sizeof(m.accel), seq);
    m.accel.samples = (uint16_t)number(argv[2], 0, PBL_ADAPTER_SENSOR_SAMPLES, &bad);
    m.accel.interval_us = (uint32_t)number(argv[3], 0, PBL_ADAPTER_ACCEL_MAX_INTERVAL_US, &bad);
    size = sizeof(m.accel);
  } else if (!strcmp(command, "hrm") && argc == 3) {
    pbl_adapter_init(&m, PBL_ADAPTER_HRM_CONFIG, sizeof(m.hrm), seq);
    m.hrm.mode = (uint8_t)number(argv[2], 0, 1, &bad);
    size = sizeof(m.hrm);
  } else if (!strcmp(command, "idle") && (argc == 3 || argc == 4)) {
    pbl_adapter_init(&m, PBL_ADAPTER_IDLE, sizeof(m.idle), seq);
    m.idle.busy = (uint8_t)number(argv[2], 0, 1, &bad);
    if (argc == 4) {
      char *end;
      errno = 0;
      m.idle.next_deadline_us = strtoull(argv[3], &end, 10);
      if (errno || end == argv[3] || *end ||
          strspn(argv[3], "0123456789") != strlen(argv[3]))
        bad = 1;
    }
    size = sizeof(m.idle);
  } else if (!strcmp(command, "trace") && (argc == 3 || argc == 4)) {
    pbl_adapter_init(&m, PBL_ADAPTER_TRACE, sizeof(m.trace), seq);
    m.trace.enable = (uint8_t)number(argv[2], 0, 1, &bad);
    m.trace.seconds = argc == 4 ? (uint16_t)number(argv[3], 1, 300, &bad) : 0;
    if (m.trace.enable && !m.trace.seconds)
      bad = 1;
    size = sizeof(m.trace);
  } else {
    return usage();
  }
  if (bad)
    return usage();
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  int result = fd < 0 ? -errno : pbl_adapter_send(fd, &peer, &m, size);
  printf("BRIDGE_COMMAND %s result=%d\n", command, result);
  return result != 0;
}
