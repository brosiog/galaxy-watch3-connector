/* SPDX-License-Identifier: Apache-2.0 */

/* Checks that a device adapter follows the v2 protocol. It plays the OS: binds os.sock, says
 * HELLO and exercises the adapter. Run it against a live adapter with the OS stopped.
 *
 *   conformance --dir RUNTIME_DIR [--bound-ms N] [--full]
 *
 * --full also checks the sensor stream and the suspend messages, which need real hardware to
 * produce data. Exit status is 0 when nothing failed; skipped checks are not failures. */

#include <stdbool.h>
#include <string.h>

#include <pbl/drivers/linux_adapter.h>

#include <poll.h>

static int s_fd = -1;
static struct sockaddr_un s_peer;
static unsigned s_failed, s_passed, s_skipped;
static unsigned s_bound_ms = 3000;
static uint32_t s_seq;
static struct pbl_adapter_caps s_caps;

static void report(bool ok, const char *name) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
  ok ? s_passed++ : s_failed++;
}

static void skip(const char *name, const char *why) {
  printf("SKIP  %s (%s)\n", name, why);
  s_skipped++;
}

static uint64_t now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

// Waits for the next valid datagram of `type`, ignoring others. Returns its size or -1.
static ssize_t wait_for(uint16_t type, void *out, size_t capacity, unsigned timeout_ms) {
  uint64_t end = now_ms() + timeout_ms;
  uint8_t buffer[sizeof(struct pbl_adapter_sensor) + 64];
  while (now_ms() < end) {
    struct pollfd p = {.fd = s_fd, .events = POLLIN};
    if (poll(&p, 1, 20) <= 0)
      continue;
    ssize_t count = recv(s_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    struct pbl_adapter_hdr hdr;
    if (count < 0 || !pbl_adapter_accept(buffer, (size_t)count, &hdr) || hdr.type != type)
      continue;
    memcpy(out, buffer, hdr.length < capacity ? hdr.length : capacity);
    return hdr.length;
  }
  return -1;
}

static void drain(void) {
  uint8_t buffer[sizeof(struct pbl_adapter_sensor) + 64];
  while (recv(s_fd, buffer, sizeof(buffer), MSG_DONTWAIT) >= 0)
    ;
}

static uint32_t send_hello(void) {
  struct pbl_adapter_hello hello;
  pbl_adapter_init(&hello, PBL_ADAPTER_HELLO, sizeof(hello), ++s_seq);
  pbl_adapter_copy_name(hello.os_build, sizeof(hello.os_build), "conformance");
  hello.max_minor = PBL_ADAPTER_MINOR;
  pbl_adapter_send(s_fd, &s_peer, &hello, sizeof(hello));
  return s_seq;
}

static bool handshake(struct pbl_adapter_caps *caps, uint32_t *seq) {
  drain();
  *seq = send_hello();
  return wait_for(PBL_ADAPTER_CAPS, caps, sizeof(*caps), s_bound_ms) >= (ssize_t)sizeof(*caps);
}

static void check_handshake(void) {
  uint32_t seq;
  bool got = handshake(&s_caps, &seq);
  report(got, "HELLO is answered with CAPS");
  if (!got) {
    printf("      nothing from %s; is the adapter running and the OS stopped?\n", s_peer.sun_path);
    return;
  }
  report(s_caps.ack_seq == seq, "CAPS echoes the HELLO seq in ack_seq");
  report(s_caps.hdr.major == PBL_ADAPTER_MAJOR, "CAPS carries major version 2");
  report(s_caps.hdr.length >= sizeof(s_caps), "CAPS has its full length");
  report(s_caps.adapter_name[0] && memchr(s_caps.adapter_name, 0, sizeof(s_caps.adapter_name)),
         "adapter_name is set and terminated");
  report(memchr(s_caps.adapter_version, 0, sizeof(s_caps.adapter_version)) != NULL,
         "adapter_version is terminated");
  report(s_caps.caps != 0, "at least one capability is declared");
  if (s_caps.caps & PBL_ADAPTER_CAP_ACCEL)
    report(s_caps.accel_min_interval_us >= 1000 &&
               s_caps.accel_min_interval_us <= s_caps.accel_max_interval_us &&
               s_caps.accel_max_interval_us <= PBL_ADAPTER_ACCEL_MAX_INTERVAL_US &&
               s_caps.accel_max_samples >= 1 &&
               s_caps.accel_max_samples <= PBL_ADAPTER_SENSOR_SAMPLES,
           "accelerometer limits are sane");
  if (s_caps.caps & PBL_ADAPTER_CAP_VIBE)
    report(
        s_caps.vibe_max_duration_ms >= 1 && s_caps.vibe_max_duration_ms <= PBL_ADAPTER_VIBE_MAX_MS,
        "vibe_max_duration_ms is within 1..5000");
  struct pbl_adapter_state state;
  bool got_state =
      wait_for(PBL_ADAPTER_STATE, &state, sizeof(state), s_bound_ms) >= (ssize_t)sizeof(state);
  report(got_state, "STATE follows CAPS");
  if (got_state)
    report(state.display_actual >= PBL_ADAPTER_DISPLAY_ON &&
               state.display_actual <= PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK,
           "STATE.display_actual is a known mode");
}

static void check_robustness(void) {
  struct pbl_adapter_hdr bad;
  pbl_adapter_init(&bad, 0x00ee, sizeof(bad), ++s_seq);
  pbl_adapter_send(s_fd, &s_peer, &bad, sizeof(bad));
  pbl_adapter_init(&bad, PBL_ADAPTER_HELLO, sizeof(bad), ++s_seq);
  bad.magic = 0x1111;
  pbl_adapter_send(s_fd, &s_peer, &bad, sizeof(bad));
  pbl_adapter_init(&bad, PBL_ADAPTER_HELLO, sizeof(bad), ++s_seq);
  bad.major = 9;
  pbl_adapter_send(s_fd, &s_peer, &bad, sizeof(bad));
  // A DISPLAY cut short, and one with an invalid mode.
  struct pbl_adapter_display display;
  pbl_adapter_init(&display, PBL_ADAPTER_DISPLAY, sizeof(display), ++s_seq);
  pbl_adapter_send(s_fd, &s_peer, &display, sizeof(display) - 2);
  display.mode = 77;
  pbl_adapter_send(s_fd, &s_peer, &display, sizeof(display));
  pbl_adapter_send(s_fd, &s_peer, "junk", 4);
  struct pbl_adapter_caps caps;
  uint32_t seq;
  report(handshake(&caps, &seq) && caps.ack_seq == seq,
         "unknown, malformed and short datagrams are ignored");
  // The first valid HELLO after malformed ones must not have been consumed as a reply to them.
}

static bool request_display(unsigned mode, unsigned reason, uint32_t *seq_out, uint64_t *latency) {
  struct pbl_adapter_display display;
  pbl_adapter_init(&display, PBL_ADAPTER_DISPLAY, sizeof(display), ++s_seq);
  display.mode = (uint8_t)mode;
  display.reason = (uint8_t)reason;
  *seq_out = s_seq;
  uint64_t start = now_ms();
  pbl_adapter_send(s_fd, &s_peer, &display, sizeof(display));
  uint64_t end = start + s_bound_ms;
  while (now_ms() < end) {
    struct pbl_adapter_state state;
    if (wait_for(PBL_ADAPTER_STATE, &state, sizeof(state), 100) < (ssize_t)sizeof(state))
      continue;
    if (state.ack_seq == s_seq && state.display_actual == mode) {
      *latency = now_ms() - start;
      return true;
    }
  }
  return false;
}

static void check_display(void) {
  if (!(s_caps.caps & PBL_ADAPTER_CAP_DISPLAY_POWER)) {
    skip("STATE reflects DISPLAY", "adapter has no DISPLAY_POWER");
    return;
  }
  static const struct {
    unsigned mode, reason;
    const char *name;
  } steps[] = {
    {PBL_ADAPTER_DISPLAY_OFF, PBL_ADAPTER_REASON_TIMEOUT, "DISPLAY OFF"},
    {PBL_ADAPTER_DISPLAY_ON, PBL_ADAPTER_REASON_USER, "DISPLAY ON"},
  };
  for (unsigned i = 0; i < sizeof(steps) / sizeof(*steps); i++) {
    uint32_t seq;
    uint64_t latency = 0;
    char name[96];
    bool ok = request_display(steps[i].mode, steps[i].reason, &seq, &latency);
    snprintf(name, sizeof(name), "%s is reflected in STATE with ack_seq within %u ms (took %llu)",
             steps[i].name, s_bound_ms, (unsigned long long)latency);
    report(ok, name);
  }
  if (s_caps.caps & PBL_ADAPTER_CAP_AMBIENT_LOWPOWER) {
    uint32_t seq;
    uint64_t latency;
    report(request_display(PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK, PBL_ADAPTER_REASON_TIMEOUT, &seq,
                           &latency),
           "DISPLAY AMBIENT_CLOCK is reflected in STATE");
    request_display(PBL_ADAPTER_DISPLAY_ON, PBL_ADAPTER_REASON_USER, &seq, &latency);
  }
  // The adapter never turns the panel off on its own.
  struct pbl_adapter_state state;
  uint64_t end = now_ms() + 1500;
  bool stayed = true;
  while (now_ms() < end)
    if (wait_for(PBL_ADAPTER_STATE, &state, sizeof(state), 200) >= (ssize_t)sizeof(state) &&
        state.display_actual != PBL_ADAPTER_DISPLAY_ON)
      stayed = false;
  report(stayed, "the adapter leaves the panel on until the OS says otherwise");
}

static void check_sensors(void) {
  if (!(s_caps.caps & PBL_ADAPTER_CAP_ACCEL)) {
    skip("accelerometer stream", "adapter has no ACCEL");
    return;
  }
  struct pbl_adapter_accel_config config;
  pbl_adapter_init(&config, PBL_ADAPTER_ACCEL_CONFIG, sizeof(config), ++s_seq);
  config.samples = s_caps.accel_max_samples < 25 ? s_caps.accel_max_samples : 25;
  config.interval_us = s_caps.accel_min_interval_us > 40000 ? s_caps.accel_min_interval_us : 40000;
  if (config.interval_us > s_caps.accel_max_interval_us)
    config.interval_us = s_caps.accel_max_interval_us;
  drain();
  pbl_adapter_send(s_fd, &s_peer, &config, sizeof(config));
  struct pbl_adapter_sensor sensor;
  ssize_t size = -1;
  uint64_t end = now_ms() + 10000;
  while (now_ms() < end) {
    size = wait_for(PBL_ADAPTER_SENSOR, &sensor, sizeof(sensor), 100);
    if (size >= (ssize_t)sizeof(sensor) && sensor.type == PBL_ADAPTER_SENSOR_ACCEL)
      break;
    size = -1;
  }
  report(size >= (ssize_t)sizeof(sensor), "ACCEL_CONFIG starts SENSOR packets of full size");
  if (size >= (ssize_t)sizeof(sensor)) {
    report(sensor.type == PBL_ADAPTER_SENSOR_ACCEL, "SENSOR type is ACCEL");
    report(sensor.count >= 1 && sensor.count <= PBL_ADAPTER_SENSOR_SAMPLES,
           "SENSOR count is within 1..125");
    report(sensor.interval_us >= PBL_ADAPTER_ACCEL_MIN_INTERVAL_US &&
               sensor.interval_us <= PBL_ADAPTER_ACCEL_MAX_INTERVAL_US,
           "SENSOR interval is within 1 ms..1 s");
    report(sensor.timestamp_us <= pbl_adapter_boottime_us() &&
               pbl_adapter_boottime_us() - sensor.timestamp_us < 30000000,
           "SENSOR timestamp is recent CLOCK_BOOTTIME");
  }
  config.samples = 0;
  config.hdr.seq = ++s_seq;
  pbl_adapter_send(s_fd, &s_peer, &config, sizeof(config));
}

static void check_suspend(void) {
  if (!(s_caps.caps & PBL_ADAPTER_CAP_SUSPEND)) {
    skip("IDLE accepted", "adapter has no SUSPEND");
    return;
  }
  struct pbl_adapter_idle idle;
  pbl_adapter_init(&idle, PBL_ADAPTER_IDLE, sizeof(idle), ++s_seq);
  idle.busy = 1;
  pbl_adapter_send(s_fd, &s_peer, &idle, sizeof(idle));
  idle.busy = 0;
  idle.next_deadline_us = pbl_adapter_boottime_us() + 60000000;
  pbl_adapter_send(s_fd, &s_peer, &idle, sizeof(idle));
  idle.next_deadline_us = 0;
  pbl_adapter_send(s_fd, &s_peer, &idle, sizeof(idle));
  struct pbl_adapter_caps caps;
  uint32_t seq;
  report(handshake(&caps, &seq), "adapter still answers after IDLE busy/idle/cancel");
}

static void check_vibe(void) {
  if (!(s_caps.caps & PBL_ADAPTER_CAP_VIBE)) {
    skip("VIBE accepted", "adapter has no VIBE");
    return;
  }
  // An expired pulse must not buzz. A stop must always be accepted.
  struct pbl_adapter_vibe vibe;
  pbl_adapter_init(&vibe, PBL_ADAPTER_VIBE, sizeof(vibe), ++s_seq);
  vibe.strength = 100;
  vibe.duration_ms = 500;
  vibe.deadline_ms = 1;
  pbl_adapter_send(s_fd, &s_peer, &vibe, sizeof(vibe));
  pbl_adapter_build_vibe(&vibe, ++s_seq, 0, 0);
  pbl_adapter_send(s_fd, &s_peer, &vibe, sizeof(vibe));
  struct pbl_adapter_caps caps;
  uint32_t seq;
  report(handshake(&caps, &seq), "adapter still answers after an expired VIBE and a stop");
}

static void check_restart_announcement(void) {
  // Informational: an adapter sends ADAPTER_READY when it starts, which this run cannot trigger.
  skip("ADAPTER_READY on start", "needs an adapter restart; covered by the OS client tests");
}

int main(int argc, char **argv) {
  const char *dir = NULL;
  bool full = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dir") && i + 1 < argc)
      dir = argv[++i];
    else if (!strcmp(argv[i], "--bound-ms") && i + 1 < argc)
      s_bound_ms = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--full"))
      full = true;
    else {
      fprintf(stderr, "usage: %s --dir RUNTIME_DIR [--bound-ms N] [--full]\n", argv[0]);
      return 2;
    }
  }
  if (!dir) {
    fprintf(stderr, "usage: %s --dir RUNTIME_DIR [--bound-ms N] [--full]\n", argv[0]);
    return 2;
  }
  int rc = pbl_adapter_socket_address(&s_peer, dir, PBL_ADAPTER_ADAPTER_SOCK);
  s_fd = rc ? rc : pbl_adapter_bind(dir, PBL_ADAPTER_OS_SOCK);
  if (s_fd < 0) {
    fprintf(stderr, "cannot bind %s/%s: %s (is the OS running?)\n", dir, PBL_ADAPTER_OS_SOCK,
            strerror(-s_fd));
    return 2;
  }
  check_handshake();
  if (s_caps.caps) {
    check_robustness();
    check_display();
    check_vibe();
    if (full) {
      check_sensors();
      check_suspend();
    } else {
      skip("sensor stream and IDLE", "run with --full");
    }
    check_restart_announcement();
  }
  char path[256];
  if (!pbl_adapter_join(path, sizeof(path), dir, PBL_ADAPTER_OS_SOCK))
    unlink(path);
  close(s_fd);
  printf("%u passed, %u failed, %u skipped\n", s_passed, s_failed, s_skipped);
  return s_failed != 0;
}
