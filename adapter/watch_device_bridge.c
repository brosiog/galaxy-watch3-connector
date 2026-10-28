/* ARM32 adapter for the installed Tizen sensor and haptic APIs. */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <pbl/drivers/linux_adapter.h>

#define ADAPTER_NAME "galaxy-watch3"
#define ADAPTER_VERSION "2.0"
// The Galaxy Watch3 profile's runtime directory. PBL_RUNTIME_DIR or XDG_RUNTIME_DIR override it.
#define GALAXY_RUNTIME_DIR "/opt/usr/home/owner/share/pebble"
#define BRIGHTNESS_PATH "/sys/class/backlight/s6e36w4x01-bl/brightness"
static char s_dir[128];
static char s_trace_path[256];
// Private to this adapter and its FIFO helper; the OS never reads it.
static char s_policy_path[256];
static struct sockaddr_un s_os;
static uint32_t s_seq;
#define TRACE_MAX_BYTES (1024 * 1024)
// Screen-off accelerometer interval. 5 Hz (200 ms) would be enough for Pebble's sleep tracking,
// but on this hub it streams every sample instead of buffering while the AP sleeps (bbd held
// 60-90% of the time vs 13% at 40 ms), so the rate stays at Pebble's request.
#define SCREEN_OFF_INTERVAL_MS 40
// Starting the pedometer listener sends the hub six configuration commands, so with the screen
// off it is read only every 2 minutes (Pebble times out a silent pedometer after 5).
#define STEPS_POLL_S 15
#define STEPS_POLL_SCREEN_OFF_S 120
#define MAX_BATCH_LATENCY_MS 5000
#define STEPS_HEARTBEAT_S 60
// With no OS on os.sock this long, the display goes off instead of staying on forever.
#define OS_GONE_GRACE_S 15
static struct {
  int fd;
  size_t bytes;
  uint64_t deadline;
} s_trace = {.fd = -1};
static uint64_t trace_clock(void) {
  struct timespec now;
  clock_gettime(CLOCK_BOOTTIME, &now);
  return (uint64_t)now.tv_sec;
}
static void trace_stop(const char *reason) {
  if (s_trace.fd < 0)
    return;
  close(s_trace.fd);
  s_trace.fd = -1;
  printf("SENSOR_TRACE_STOP reason=%s bytes=%zu\n", reason, s_trace.bytes);
}
static int trace_start(const char *path, unsigned seconds) {
  if (!seconds || seconds > 300)
    return -EINVAL;
  trace_stop("replaced");
  s_trace.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (s_trace.fd < 0)
    return -errno;
  s_trace.bytes = 0;
  s_trace.deadline = trace_clock() + seconds;
  printf("SENSOR_TRACE_START seconds=%u max_bytes=%u\n", seconds, TRACE_MAX_BYTES);
  return 0;
}
static void trace_packet(const struct pbl_adapter_sensor *packet) {
  if (s_trace.fd < 0)
    return;
  if (trace_clock() >= s_trace.deadline) {
    trace_stop("deadline");
    return;
  }
  if (s_trace.bytes + sizeof(*packet) > TRACE_MAX_BYTES) {
    trace_stop("size_limit");
    return;
  }
  ssize_t written = write(s_trace.fd, packet, sizeof(*packet));
  if (written != sizeof(*packet)) {
    trace_stop("write_error");
    return;
  }
  s_trace.bytes += written;
}
/* Samsung/TizenFX API7 Interop.Sensor.cs: sequential native sensor_event_s. */
struct sensor_event {
  int accuracy;
  uint64_t timestamp;
  int count;
  float values[16];
};
_Static_assert(__builtin_offsetof(struct sensor_event, values) == 20,
               "sensor ABI");
_Static_assert(sizeof(struct sensor_event) == 88, "sensor array stride");
static struct {
  int (*get)(const char *, void **);
  int (*create)(void *, void **);
  int (*set_cb)(void *, unsigned, void (*)(void *, void *, void *), void *);
  int (*set_events_cb)(void *, void (*)(void *, struct sensor_event *, int, void *), void *);
  int (*set_interval)(void *, unsigned);
  int (*option)(void *, int);
  int (*batch)(void *, unsigned);
  int (*start)(void *);
  int (*stop)(void *);
  int (*destroy)(void *);
  void *listener[2];
  unsigned interval_ms, samples;
  // Pebble's request; interval_ms differs from it while the screen-off rate applies.
  unsigned requested_samples, requested_interval_ms;
  // Mean spacing of delivered events, which other listeners can make faster than requested.
  unsigned measured_interval_ms;
  uint64_t accel_last_raw;
  bool running[2];
  int fd;
  unsigned batches, dropped;
  uint64_t accel_last_event, accel_last_timestamp;
  unsigned max_callback_events;
  struct pbl_adapter_sensor accel;
} s_health = {.fd = -1};
static bool s_policy_dirty;
// The sensor hub can wedge and repeat one accelerometer sample forever (raise-to-wake and steps
// stop). Real samples always carry noise, so flag two consecutive single-valued packets, and
// again every 6 packets (30 s) while it stays frozen. The FIFO helper restarts the sensor
// transport when the count in its policy file rises; the OS is told through FAULT.
static unsigned s_frozen_packets;
static unsigned s_frozen_events;
static void send_fault(unsigned component, unsigned state);
static void check_frozen(const struct pbl_adapter_sensor *packet) {
  bool frozen = packet->count >= PBL_ADAPTER_SENSOR_SAMPLES;
  for (unsigned i = 1; frozen && i < packet->count; i++)
    frozen = !memcmp(packet->values[i], packet->values[0], sizeof(packet->values[0]));
  unsigned previous = s_frozen_packets;
  s_frozen_packets = frozen ? s_frozen_packets + 1 : 0;
  if (s_frozen_packets >= 2 && (s_frozen_packets - 2) % 6 == 0) {
    s_frozen_events++;
    s_policy_dirty = true;
    send_fault(PBL_ADAPTER_COMPONENT_ACCEL, PBL_ADAPTER_FAULT_RECOVERING);
    printf("BRIDGE_ACCEL_FROZEN value=%d,%d,%d events=%u\n", packet->values[0][0],
           packet->values[0][1], packet->values[0][2], s_frozen_events);
  } else if (previous >= 2 && !frozen) {
    send_fault(PBL_ADAPTER_COMPONENT_ACCEL, PBL_ADAPTER_FAULT_OK);
  }
}

// Starts a sensor message; the caller fills the type and rows.
static struct pbl_adapter_sensor new_sensor(unsigned type, unsigned count, unsigned interval_us,
                                            uint64_t timestamp_us) {
  struct pbl_adapter_sensor packet;
  memset(&packet, 0, sizeof(packet));
  packet.type = type;
  packet.count = count;
  packet.interval_us = interval_us;
  packet.timestamp_us = timestamp_us;
  return packet;
}
static void send_health(struct pbl_adapter_sensor *packet) {
  packet->hdr = (struct pbl_adapter_hdr){.magic = PBL_ADAPTER_MAGIC,
                                         .major = PBL_ADAPTER_MAJOR,
                                         .minor = PBL_ADAPTER_MINOR,
                                         .type = PBL_ADAPTER_SENSOR,
                                         .length = sizeof(*packet),
                                         .seq = ++s_seq};
  trace_packet(packet);
  if (pbl_adapter_send(s_health.fd, &s_os, packet, sizeof(*packet)))
    s_health.dropped++;
}
// Tizen SensorDataAccuracy (bad 0, normal 1, good 2, very good 3) to Pebble HRMQuality. The OS
// receives Pebble units only.
static int hrm_quality(int bpm, int accuracy) {
  if (bpm == -3)
    return -1;
  if (bpm < 25 || bpm > 240)
    return 0;
  return accuracy >= 3 ? 4 : accuracy == 2 ? 3 : accuracy == 1 ? 2 : 0;
}

static void health_event(void *sensor, void *data, void *context) {
  (void)sensor;
  const struct sensor_event *event=data;
  unsigned index=(uintptr_t)context;
  if (!event || !s_health.running[index] || event->count < (index ? 1 : 3) || event->count > 16)
    return;
  if (index == 1) {
    if (!isfinite(event->values[0]) || event->values[0] < -10 || event->values[0] > 240)
      return;
    struct pbl_adapter_sensor packet=new_sensor(PBL_ADAPTER_SENSOR_HRM, 1, 1000000,
                                                event->timestamp);
    packet.values[0][0]=lroundf(event->values[0]);
    packet.values[0][1]=hrm_quality(packet.values[0][0], event->accuracy);
    send_health(&packet);
    // Readings repeat at 1 Hz while acquiring; log changes only.
    static int logged_bpm = -1000, logged_accuracy = -1;
    if (packet.values[0][0] != logged_bpm || event->accuracy != logged_accuracy) {
      logged_bpm = packet.values[0][0];
      logged_accuracy = event->accuracy;
      printf("BRIDGE_HRM bpm=%d accuracy=%d\n", logged_bpm, logged_accuracy);
    }
    return;
  }
  for (unsigned axis=0;axis<3;axis++)
    if (!isfinite(event->values[axis]) || fabsf(event->values[axis]) > 320)
      return;
  struct pbl_adapter_sensor *packet=&s_health.accel;
  uint64_t previous = s_health.accel_last_timestamp;
  uint64_t interval = s_health.interval_ms * 1000;
  if (previous && event->timestamp <= previous)
    return;
  if (s_health.accel_last_raw && event->timestamp > s_health.accel_last_raw) {
    uint64_t spacing = event->timestamp - s_health.accel_last_raw;
    if (spacing < 2000000) {
      unsigned ms = (spacing + 500) / 1000;
      s_health.measured_interval_ms = s_health.measured_interval_ms
          ? (s_health.measured_interval_ms * 7 + ms + 4) / 8 : ms;
    }
  }
  s_health.accel_last_raw = event->timestamp;
  // Other listeners can raise the hub's rate above ours; keep the requested spacing.
  if (previous && event->timestamp - previous < interval * 9 / 10)
    return;
  if (packet->count && event->timestamp - previous > interval + interval / 2) {
    // Preserve the earlier samples without inventing samples across a transport gap.
    send_health(packet);
    packet->count = 0;
  }
  s_health.accel_last_timestamp = event->timestamp;
  s_health.accel_last_event = trace_clock();
  if (!packet->count) {
    packet->type=PBL_ADAPTER_SENSOR_ACCEL;
    packet->interval_us=s_health.interval_ms * 1000;
    packet->timestamp_us=event->timestamp;
  }
  for (unsigned axis=0;axis<3;axis++) {
    int mg=lroundf(event->values[axis] * (1000.0f / 9.80665f));
    if (mg < -32767) mg=-32767;
    if (mg > 32767) mg=32767;
    packet->values[packet->count][axis]=axis==2 ? -mg : mg;
  }
  if (++packet->count >= s_health.samples) {
    check_frozen(packet);
    send_health(packet);
    if (++s_health.batches % 60 == 1)
      printf("BRIDGE_ACCEL_BATCH count=%u interval_us=%u batches=%u dropped=%u\n",
             packet->count,packet->interval_us,s_health.batches,s_health.dropped);
    packet->count=0;
  }
}

static void health_events(void *sensor, struct sensor_event *events, int count, void *context) {
  if (!events || count <= 0)
    return;
  if (!(uintptr_t)context && (unsigned)count > s_health.max_callback_events) {
    s_health.max_callback_events = count;
    printf("BRIDGE_ACCEL_CALLBACK_MAX events=%d\n", count);
  }
  for (int i = 0; i < count; i++)
    health_event(sensor, &events[i], context);
}

static void configure_health(unsigned index, unsigned samples, unsigned interval_ms) {
  if (index > 1 || samples > PBL_ADAPTER_SENSOR_SAMPLES || interval_ms < 10 || interval_ms > 1000)
    return;
  bool enable=samples>0;
  if (enable == s_health.running[index] &&
      (index || (samples==s_health.samples && interval_ms==s_health.interval_ms)))
    return;
  if (s_health.running[index]) {
    s_health.stop(s_health.listener[index]);
    s_health.running[index]=false;
  }
  // Rate changes follow the screen; keep the partial batch.
  if (!index && s_health.accel.count)
    send_health(&s_health.accel);
  if (!index) {
    s_health.samples=samples;
    s_health.interval_ms=interval_ms;
    s_health.accel.count=0;
    s_health.accel_last_timestamp=0;
    s_health.accel_last_raw=0;
    s_health.measured_interval_ms=0;
    s_health.accel_last_event=trace_clock();
  }
  int rc=0;
  if (enable) {
    if (!s_health.listener[index]) {
      const char *uri=index ? "http://tizen.org/sensor/healthinfo/heart_rate_monitor/SENSOR_HRM" :
        "http://tizen.org/sensor/general/accelerometer/SENSOR_ACCELEROMETER";
      void *sensor=NULL;
      rc=s_health.get(uri,&sensor);
      if (!rc) rc=s_health.create(sensor,&s_health.listener[index]);
    }
    bool multi = !index && s_health.set_events_cb && s_health.set_interval;
    if (!rc && multi) {
      rc=s_health.set_events_cb(s_health.listener[index],health_events,NULL);
      if (!rc) rc=s_health.set_interval(s_health.listener[index],interval_ms);
    } else if (!rc) {
      rc=s_health.set_cb(s_health.listener[index],interval_ms,health_event,(void*)(uintptr_t)index);
    }
    if (!rc) rc=s_health.option(s_health.listener[index],1);
    // The hub streams every sample (latency 0) if asked for more than 5 s; the FIFO helper does
    // the long buffering.
    unsigned latency_ms = samples * interval_ms < MAX_BATCH_LATENCY_MS ? samples * interval_ms
                                                                       : MAX_BATCH_LATENCY_MS;
    if (!rc && !index && s_health.batch)
      printf("BRIDGE_ACCEL_BATCH_LATENCY ms=%u result=%d\n",latency_ms,
             s_health.batch(s_health.listener[index],latency_ms));
    if (!rc) rc=s_health.start(s_health.listener[index]);
    s_health.running[index]=rc==0;
  }
  printf("BRIDGE_SENSOR_CONFIG index=%u enabled=%d samples=%u interval_ms=%u result=%d\n",
         index,s_health.running[index],samples,interval_ms,rc);
  s_policy_dirty = true;
}
// The hub pedometer is a wake-up sensor that reports every step change; a running listener would
// wake the AP while walking. Its current total is read instead whenever the AP is awake.
static struct {
  void *listener;
  int (*read)(void *, struct sensor_event *);
  bool have_total;
  long total;
  uint64_t last_poll, last_sent;
} s_steps;
static void open_steps(void) {
  void *sensor = NULL;
  int rc = s_steps.read ? s_health.get("http://tizen.org/sensor/healthinfo/human_pedometer",
                                       &sensor) : -ENOSYS;
  if (!rc)
    rc = s_health.create(sensor, &s_steps.listener);
  if (!rc)
    rc = s_health.option(s_steps.listener, 1);
  if (rc && s_steps.listener) {
    s_health.destroy(s_steps.listener);
    s_steps.listener = NULL;
  }
  printf("BRIDGE_STEPS_OPEN=%d\n", rc);
}
static void poll_steps(uint64_t now, bool ambient) {
  unsigned period = ambient ? STEPS_POLL_SCREEN_OFF_S : STEPS_POLL_S;
  if (!s_steps.listener || (s_steps.last_poll && now - s_steps.last_poll < period))
    return;
  struct sensor_event event = {0};
  int rc = s_health.start(s_steps.listener);
  if (!rc)
    rc = s_steps.read(s_steps.listener, &event);
  s_health.stop(s_steps.listener);
  if (rc || event.count < 1 || !isfinite(event.values[0]) || event.values[0] < 0)
    return;
  long total = lroundf(event.values[0]);
  long delta = 0;
  if (s_steps.have_total)
    delta = total >= s_steps.total ? total - s_steps.total : total;  // daily reset
  uint64_t elapsed = s_steps.last_poll ? now - s_steps.last_poll : 0;
  s_steps.total = total;
  s_steps.have_total = true;
  s_steps.last_poll = now;
  if (!delta && s_steps.last_sent && now - s_steps.last_sent < STEPS_HEARTBEAT_S)
    return;
  if (delta > 30000)
    delta = 30000;
  // Pebble derives stride from cadence; keep it between 60 and 180 steps per minute.
  if (elapsed > (uint64_t)delta)
    elapsed = delta;
  if (elapsed < (uint64_t)(delta + 2) / 3)
    elapsed = (delta + 2) / 3;
  struct pbl_adapter_sensor packet =
      new_sensor(PBL_ADAPTER_SENSOR_STEPS, 1, 1000000, now * 1000000);
  packet.values[0][0] = delta;
  packet.values[0][1] = elapsed;
  send_health(&packet);
  s_steps.last_sent = now;
  if (delta)
    printf("BRIDGE_STEPS total=%ld delta=%ld elapsed_s=%llu\n", total, delta,
           (unsigned long long)elapsed);
}
// Pebble's activity tracking alone (125 samples at 40 ms, no HR) drops to the screen-off rate.
// Pebble re-sends its request every 120 s, which also retries a failed configuration.
static void update_accel_rate(bool ambient, bool requested) {
  static unsigned last_samples, last_interval;
  unsigned samples = s_health.requested_samples, interval = s_health.requested_interval_ms;
  if (!interval)
    return;
  if (ambient && samples == PBL_ADAPTER_SENSOR_SAMPLES && interval == 40 && !s_health.running[1] &&
      s_steps.have_total)
    interval = SCREEN_OFF_INTERVAL_MS;
  if (!requested && samples == last_samples && interval == last_interval)
    return;
  last_samples = samples;
  last_interval = interval;
  configure_health(0, samples, interval);
}
// The FIFO helper sizes its flush cycle from this; until a rate is measured, assume the request
// is not honored (40 ms keeps its cycle short and the hub FIFO safe).
static unsigned published_interval(void) {
  unsigned interval = s_health.interval_ms;
  if (interval > 40 && (!s_health.measured_interval_ms || s_health.measured_interval_ms < interval))
    return s_health.measured_interval_ms > 40 ? s_health.measured_interval_ms : 40;
  return interval;
}
static void recover_health(uint64_t now) {
  // A service restart can leave the CAPI listener alive without delivering events.
  // Slow (5 Hz) batches arrive about every 20 s.
  unsigned stall = s_health.interval_ms >= 200 ? 60 : 30;
  if (!s_health.samples || now - s_health.accel_last_event < stall)
    return;
  unsigned samples = s_health.samples, interval = s_health.interval_ms;
  printf("BRIDGE_ACCEL_RECOVERY stalled_seconds=%llu running=%d\n",
         (unsigned long long)(now - s_health.accel_last_event), s_health.running[0]);
  if (s_health.listener[0]) {
    if (s_health.running[0])
      s_health.stop(s_health.listener[0]);
    s_health.destroy(s_health.listener[0]);
    s_health.listener[0] = NULL;
  }
  s_health.running[0] = false;
  configure_health(0, samples, interval);
}
static volatile sig_atomic_t stopped, wrist_action, lcd_signal;
static unsigned light_lux;
static bool light_valid;
static void light_event(void *sensor, void *data, void *context) {
  (void)sensor;
  (void)context;
  const struct sensor_event *event = data;
  if (event && event->count > 0 && event->count <= 16 &&
      isfinite(event->values[0]) && event->values[0] >= 0) {
    float lux = event->values[0];
    light_lux = lux > 65535 ? 65535 : (unsigned)lux;
    light_valid = true;
  }
}
static int read_brightness(void) {
  FILE *f = fopen(BRIGHTNESS_PATH, "r");
  int value = 60;
  if (f) {
    if (fscanf(f, "%d", &value) != 1)
      value = 60;
    fclose(f);
  }
  return value;
}
static int panel_power(void) {
  FILE *f = fopen("/sys/class/lcd/s6e36w4x01/lcd_power", "r");
  int value = -1;
  if (f) {
    if (fscanf(f, "%d", &value) != 1)
      value = -1;
    fclose(f);
  }
  return value;
}
static int recover_panel_if_off(void) {
  /* Direct scanout recovery is experimental; normal wear uses Tizen power APIs.
   */
  if (!getenv("PBL_PANEL_RECOVERY"))
    return 0;
  int before = panel_power();
  if (before == 0)
    return 0;
  if (before < 0)
    return -1;
  int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -errno;
  int down = ioctl(fd, 0x4611, 4);
  struct timespec delay = {.tv_nsec = 300000000};
  nanosleep(&delay, NULL);
  int up = ioctl(fd, 0x4611, 0);
  close(fd);
  int after = panel_power();
  printf("BRIDGE_PANEL_RECOVERY before=%d down=%d up=%d after=%d\n", before,
         down, up, after);
  return down || up || after != 0 ? -1 : 0;
}
static int (*set_brightness)(int, int);
static int write_brightness(unsigned value) {
  return set_brightness ? set_brightness(0, value) : -ENOSYS;
}
// What the OS last asked the panel to do. A change that did not come from a DISPLAY message (the
// wrist-up fast path, a vendor signal) carries seq 0.
static struct {
  unsigned mode;
  uint32_t seq;
  uint32_t applied_seq;
} s_want = {.mode = PBL_ADAPTER_DISPLAY_ON};
static unsigned s_display_actual;
static bool s_display_lock;
static bool s_display_failed;
struct display_power_ops {
  int (*wake)(bool);
  int (*request_lock)(int, int);
  int (*release_lock)(int);
  int (*change_state)(int);
};
static bool s_os_alive;
static uint64_t s_wrist_timestamp;
static void send_to_os(void *message, size_t size, unsigned type) {
  struct pbl_adapter_hdr *hdr = message;
  *hdr = (struct pbl_adapter_hdr){.magic = PBL_ADAPTER_MAGIC,
                                  .major = PBL_ADAPTER_MAJOR,
                                  .minor = PBL_ADAPTER_MINOR,
                                  .type = type,
                                  .length = size,
                                  .seq = ++s_seq};
  int rc = pbl_adapter_send(s_health.fd, &s_os, message, size);
  // Nobody bound to os.sock means the OS is not running.
  s_os_alive = rc == 0 || (rc != -ECONNREFUSED && rc != -ENOENT);
}
static void send_ready(void) {
  struct pbl_adapter_hdr ready;
  send_to_os(&ready, sizeof(ready), PBL_ADAPTER_ADAPTER_READY);
}
static void send_gesture(unsigned kind) {
  struct pbl_adapter_gesture gesture;
  memset(&gesture, 0, sizeof(gesture));
  gesture.kind = kind;
  gesture.timestamp_us = s_wrist_timestamp && (kind == PBL_ADAPTER_GESTURE_WRIST_UP ||
                                               kind == PBL_ADAPTER_GESTURE_WRIST_DOWN)
                             ? s_wrist_timestamp
                             : pbl_adapter_boottime_us();
  send_to_os(&gesture, sizeof(gesture), PBL_ADAPTER_GESTURE);
}
static void send_fault(unsigned component, unsigned state) {
  struct pbl_adapter_fault fault;
  memset(&fault, 0, sizeof(fault));
  fault.component = component;
  fault.state = state;
  send_to_os(&fault, sizeof(fault), PBL_ADAPTER_FAULT);
}
static int apply_display_power(unsigned mode, bool lowpower, const struct display_power_ops *ops) {
  bool on = mode == PBL_ADAPTER_DISPLAY_ON ||
            (mode == PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK && !lowpower);
  int rc;
  if (on) {
    rc = ops->wake(false);
    if (!rc && !s_display_lock) {
      rc = ops->request_lock(1, 0);
      if (!rc)
        s_display_lock = true;
    }
  } else {
    rc = s_display_lock ? ops->release_lock(1) : 0;
    if (!rc) {
      s_display_lock = false;
      rc = ops->change_state(2);
    }
  }
  if (!rc) {
    s_display_actual = mode;
    s_want.applied_seq = s_want.seq;
  }
  if (s_display_failed != (rc != 0)) {
    s_display_failed = rc != 0;
    send_fault(PBL_ADAPTER_COMPONENT_DISPLAY,
               rc ? PBL_ADAPTER_FAULT_FAILED : PBL_ADAPTER_FAULT_OK);
  }
  return rc;
}
static void publish_state(void) {
  if (!s_display_actual)
    return;
  bool ambient = s_display_actual != PBL_ADAPTER_DISPLAY_ON;
  struct pbl_adapter_state state;
  memset(&state, 0, sizeof(state));
  state.ack_seq = s_want.applied_seq;
  state.display_actual = s_display_actual;
  state.lux = light_lux;
  state.lux_valid = light_valid;
  send_to_os(&state, sizeof(state), PBL_ADAPTER_STATE);
  // The FIFO helper's policy. The OS reads none of this, and the helper needs no OS files.
  char temporary[sizeof(s_policy_path) + 8];
  snprintf(temporary, sizeof(temporary), "%s.new", s_policy_path);
  FILE *f = fopen(temporary, "w");
  if (!f)
    return;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  fprintf(f, "2 %lld %u %u %u %u %u %u\n", (long long)now.tv_sec, ambient,
          s_health.running[0] ? s_health.samples : 0, published_interval(),
          s_health.running[1], s_os_alive, s_frozen_events);
  if (!fclose(f))
    rename(temporary, s_policy_path);
}
static int tick(void *data) {
  (void)data;
  recover_health(trace_clock());
  return 1;
}
static int io_ready(void *channel, unsigned condition, void *data) {
  (void)channel;
  (void)condition;
  (void)data;
  return 1;
}
static void stop_signal(int signal) {
  (void)signal;
  stopped = 1;
}
static uint64_t boot_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_BOOTTIME, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void wrist_event(void *sensor, void *event, void *context) {
  (void)sensor;
  const struct sensor_event *e = event;
  s_wrist_timestamp = e ? e->timestamp : 0;
  wrist_action = (uintptr_t)context;
}
// Samsung's display service turns the LCD on for its own raise and palm handling. Follow it:
// the adapter's wrist listeners can stay silent after a restart while sensord still lights the LCD.
static void lcd_signal_cb(void *connection, const char *sender, const char *path,
                          const char *interface, const char *signal, void *parameters,
                          void *data) {
  (void)connection;
  (void)sender;
  (void)path;
  (void)interface;
  (void)parameters;
  (void)data;
  lcd_signal = !strcmp(signal, "LCDOn") ? 1 : 2;
}
// Low-power idle clock, following Samsung's own AOD handshake: with the display "off", deviced
// waits in ambient mode; HomeScreen("clockend") tells it the clock frame is drawn and it enters
// its real ambient state, keeping the panel showing that frame while the AP sleeps. Each minute
// an alarm wakes the AP, "clockstart" reopens the panel for drawing, the renderer redraws, and
// "clockend" follows.
#define CLOCKEND_DELAY_MS 700
static struct {
  void *bus;
  bool (*emit)(void *, const char *, const char *, const char *, const char *, void *, void **);
  void *(*variant)(const char *, ...);
  int alarm;
  int (*vconf_set_int)(const char *, int);
  volatile sig_atomic_t minute_due, clockend_due;
} s_aod = {.alarm = -1};
static void aod_signal(const char *what) {
  if (!s_aod.bus || !s_aod.emit || !s_aod.variant)
    return;
  bool ok = s_aod.emit(s_aod.bus, NULL, "/Org/Tizen/System/DeviceD", "org.tizen.system.deviced",
                       "HomeScreen", s_aod.variant("(s)", what), NULL);
  printf("BRIDGE_AOD_%s=%d t=%llu\n", what, ok, (unsigned long long)(time(NULL)));
}
// Next local minute boundary, as a wake-up alarm.
static void aod_arm_minute(bool enable) {
  if (s_aod.alarm < 0)
    return;
  struct itimerspec timer = {0};
  if (enable) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long long ms = 60000 - (now.tv_sec % 60) * 1000 - now.tv_nsec / 1000000 + 50;
    timer.it_value.tv_sec = ms / 1000;
    timer.it_value.tv_nsec = (ms % 1000) * 1000000;
  }
  timerfd_settime(s_aod.alarm, 0, &timer, NULL);
}
static int aod_alarm_ready(void *channel, unsigned condition, void *data) {
  (void)channel;
  (void)condition;
  (void)data;
  uint64_t count;
  if (read(s_aod.alarm, &count, sizeof(count)) == sizeof(count))
    s_aod.minute_due = 1;
  return 1;
}
static int aod_clockend_timer(void *data) {
  (void)data;
  s_aod.clockend_due = 1;
  return 0;
}

static void subscribe_lcd_signals(void) {
  void *gio = dlopen("libgio-2.0.so.0", RTLD_NOW);
  void *(*bus_get)(int, void *, void **) = gio ? dlsym(gio, "g_bus_get_sync") : NULL;
  unsigned (*subscribe)(void *, const char *, const char *, const char *, const char *,
                        const char *, int, void *, void *, void *) =
      gio ? dlsym(gio, "g_dbus_connection_signal_subscribe") : NULL;
  void *bus = bus_get ? bus_get(1 /* G_BUS_TYPE_SYSTEM */, NULL, NULL) : NULL;
  s_aod.bus = bus;
  s_aod.emit = gio ? dlsym(gio, "g_dbus_connection_emit_signal") : NULL;
  void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW);
  s_aod.variant = glib ? dlsym(glib, "g_variant_new") : NULL;
  unsigned on = 0, off = 0;
  if (bus && subscribe) {
    on = subscribe(bus, NULL, "org.tizen.system.deviced.display", "LCDOn",
                   "/Org/Tizen/System/DeviceD/Display", NULL, 0, lcd_signal_cb, NULL, NULL);
    off = subscribe(bus, NULL, "org.tizen.system.deviced.display", "LCDOff",
                    "/Org/Tizen/System/DeviceD/Display", NULL, 0, lcd_signal_cb, NULL, NULL);
  }
  printf("BRIDGE_LCD_SIGNALS on=%u off=%u\n", on, off);
}

// Gesture delay from the sensor hub's timestamp (CLOCK_BOOTTIME microseconds) to this process.
static long long wrist_age_ms(void) {
  return s_wrist_timestamp ? (long long)boot_ms() - (long long)(s_wrist_timestamp / 1000) : -1;
}
static int64_t monotonic_seconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec;
}

// Set during startup from what the device and its libraries offer.
static struct {
  bool haptic, brightness, light, wrist, steps, lowpower, suspend;
} s_has;
static uint32_t adapter_caps(void) {
  uint32_t caps = PBL_ADAPTER_CAP_ACCEL | PBL_ADAPTER_CAP_HRM | PBL_ADAPTER_CAP_DISPLAY_POWER;
  if (s_has.haptic)
    caps |= PBL_ADAPTER_CAP_VIBE;
  if (s_has.brightness)
    caps |= PBL_ADAPTER_CAP_BACKLIGHT;
  if (s_has.light)
    caps |= PBL_ADAPTER_CAP_ALS;
  if (s_has.steps)
    caps |= PBL_ADAPTER_CAP_STEPS;
  if (s_has.wrist)
    caps |= PBL_ADAPTER_CAP_WRIST_GESTURE;
  if (s_has.lowpower && s_aod.bus)
    caps |= PBL_ADAPTER_CAP_AMBIENT_LOWPOWER;
  if (s_has.suspend)
    caps |= PBL_ADAPTER_CAP_SUSPEND;
  return caps;
}
static void send_caps(uint32_t ack_seq) {
  struct pbl_adapter_caps caps;
  memset(&caps, 0, sizeof(caps));
  caps.ack_seq = ack_seq;
  caps.caps = adapter_caps();
  caps.accel_min_interval_us = 10000;
  caps.accel_max_interval_us = 1000000;
  caps.accel_max_samples = PBL_ADAPTER_SENSOR_SAMPLES;
  caps.vibe_max_duration_ms = PBL_ADAPTER_VIBE_MAX_MS;
  pbl_adapter_copy_name(caps.adapter_name, sizeof(caps.adapter_name), ADAPTER_NAME);
  pbl_adapter_copy_name(caps.adapter_version, sizeof(caps.adapter_version), ADAPTER_VERSION);
  send_to_os(&caps, sizeof(caps), PBL_ADAPTER_CAPS);
}
// Wakes a suspended watch when the OS has a timer due. Its own descriptor, apart from the
// clock-minute alarm.
static int s_wake_alarm = -1;
static bool s_busy_lock;
static void arm_wake_alarm(uint64_t boottime_us) {
  if (s_wake_alarm < 0)
    return;
  struct itimerspec timer = {0};
  if (boottime_us) {
    timer.it_value.tv_sec = boottime_us / 1000000;
    timer.it_value.tv_nsec = (boottime_us % 1000000) * 1000;
    if (!timer.it_value.tv_sec && !timer.it_value.tv_nsec)
      timer.it_value.tv_nsec = 1;
  }
  timerfd_settime(s_wake_alarm, TFD_TIMER_ABSTIME, &timer, NULL);
}
static int wake_alarm_ready(void *channel, unsigned condition, void *data) {
  (void)channel;
  (void)condition;
  (void)data;
  uint64_t count;
  (void)!read(s_wake_alarm, &count, sizeof(count));
  return 1;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int seconds = argc == 2 ? atoi(argv[1]) : 1200;
  if (seconds < 30 || seconds > 86400)
    return 1;
  void *device_api = dlopen("libcapi-system-device.so.0", RTLD_NOW);
  void *sensor_api = dlopen("libcapi-system-sensor.so.0", RTLD_NOW);
  void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW);
  if (!device_api || !sensor_api || !glib)
    return 2;
  set_brightness = dlsym(device_api, "device_display_set_brightness");
  int (*wake)(bool) = dlsym(device_api, "device_power_wakeup");
  int (*change_state)(int) = dlsym(device_api, "device_display_change_state");
  int (*request_lock)(int, int) =
      dlsym(device_api, "device_power_request_lock");
  int (*release_lock)(int) = dlsym(device_api, "device_power_release_lock");
  const struct display_power_ops display_ops = {
      .wake = wake, .request_lock = request_lock,
      .release_lock = release_lock, .change_state = change_state,
  };
  int (*open_haptic)(int, void **) = dlsym(device_api, "device_haptic_open");
  int (*vibrate)(void *, int, int, void **) =
      dlsym(device_api, "device_haptic_vibrate");
  int (*stop_haptic)(void *, void *) = dlsym(device_api, "device_haptic_stop");
  int (*close_haptic)(void *) = dlsym(device_api, "device_haptic_close");
  int (*get_sensor)(const char *, void **) =
      dlsym(sensor_api, "sensor_get_default_sensor_by_uri");
  int (*create)(void *, void **) = dlsym(sensor_api, "sensor_create_listener");
  int (*set_cb)(void *, unsigned, void (*)(void *, void *, void *), void *) =
      dlsym(sensor_api, "sensor_listener_set_event_cb");
  int (*set_option)(void *, int) =
      dlsym(sensor_api, "sensor_listener_set_option");
  int (*start)(void *) = dlsym(sensor_api, "sensor_listener_start");
  int (*stop)(void *) = dlsym(sensor_api, "sensor_listener_stop");
  int (*destroy)(void *) = dlsym(sensor_api, "sensor_destroy_listener");
  int (*iterate)(void *, int) = dlsym(glib, "g_main_context_iteration");
  if (!set_brightness || !wake || !change_state || !request_lock ||
      !release_lock || !open_haptic || !vibrate || !stop_haptic ||
      !close_haptic || !get_sensor || !create || !set_cb || !set_option ||
      !start || !stop || !destroy || !iterate)
    return 3;
  s_health.get=get_sensor;
  s_health.create=create;
  s_health.set_cb=set_cb;
  s_health.set_events_cb=dlsym(sensor_api,"sensor_listener_set_events_cb");
  s_health.set_interval=dlsym(sensor_api,"sensor_listener_set_interval");
  s_health.option=set_option;
  s_health.batch=dlsym(sensor_api,"sensor_listener_set_max_batch_latency");
  s_health.start=start;
  s_health.stop=stop;
  s_health.destroy=destroy;
  s_steps.read=dlsym(sensor_api,"sensor_listener_read_data");
  open_steps();
  unsigned (*add_tick)(unsigned, int (*)(void *), void *) =
      dlsym(glib, "g_timeout_add_seconds");
  void *(*io_new)(int) = dlsym(glib, "g_io_channel_unix_new");
  unsigned (*io_watch)(void *, unsigned, int (*)(void *, unsigned, void *),
                       void *) = dlsym(glib, "g_io_add_watch");
  void (*io_unref)(void *) = dlsym(glib, "g_io_channel_unref");
  int (*remove_source)(unsigned) = dlsym(glib, "g_source_remove");
  if (!add_tick || !io_new || !io_watch || !io_unref || !remove_source)
    return 3;
  if (pbl_adapter_runtime_dir(s_dir, sizeof(s_dir), NULL, GALAXY_RUNTIME_DIR) ||
      pbl_adapter_join(s_trace_path, sizeof(s_trace_path), s_dir, "sensor-trace.bin") ||
      pbl_adapter_join(s_policy_path, sizeof(s_policy_path), s_dir, "fifo-policy") ||
      pbl_adapter_socket_address(&s_os, s_dir, PBL_ADAPTER_OS_SOCK)) {
    fprintf(stderr, "runtime directory unusable\n");
    return 3;
  }
  mkdir(s_dir, 0700);
  unsigned tick_id = 0, io_id = 0;
  void *channel = NULL;
  void *haptic = NULL, *effect = NULL, *listeners[3] = {NULL, NULL, NULL};
  int original_brightness = read_brightness();
  unsigned brightness = original_brightness > 0 ? original_brightness : 60;
  // The OS decides when the display idles; ambient and ambient_clock follow s_want.mode.
  bool ambient = false, previous_ambient = false;
  bool ambient_clock = false, previous_clock = false;
  bool clock_changed = false;
  // Experimental, opt-in with PBL_AMBIENT_LOWPOWER=1: deviced accepts our clockend but only
  // enters ALPM when Samsung's clock viewer reports an AOD-capable foreground app, so the panel
  // currently just turns off. The default keeps the display-lock clock.
  const char *lowpower_setting = getenv("PBL_AMBIENT_LOWPOWER");
  bool lowpower = lowpower_setting && !strcmp(lowpower_setting, "1");
  s_has.lowpower = lowpower;
  unsigned (*add_timeout)(unsigned, int (*)(void *), void *) = dlsym(glib, "g_timeout_add");
  s_aod.alarm = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
  void *vconf = dlopen("libvconf.so.0", RTLD_NOW);
  s_aod.vconf_set_int = vconf ? dlsym(vconf, "vconf_set_int") : NULL;
  bool aod_active = false;
  int applied_brightness = -1;
  bool display_initialized = false;
  int64_t last_publish = 0;
  int64_t last_panel_recovery = -10;
  unsigned published_lux = ~0u;
  uint32_t published_ack = 0;
  int64_t os_gone_since = 0;
  bool active = false;
  uint64_t haptic_deadline_ms = 0;
  int fd = -1, result = 0;
  bool bound = false;
  int haptic_result = open_haptic(0, &haptic);
  printf("BRIDGE_HAPTIC_OPEN=%d\n", haptic_result);
  if (haptic_result)
    return 4;
  s_has.haptic = true;
  s_has.brightness = set_brightness != NULL;
  s_has.steps = s_steps.listener != NULL;
  s_wake_alarm = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
  s_has.suspend = s_wake_alarm >= 0;
  const char *uris[] = {
      "http://tizen.org/sensor/general/gesture_wrist_up/SENSOR_WRIST_UP",
      "http://tizen.org/sensor/general/gesture_wrist_down/"
      "SENSOR_GESTURE_WRIST_DOWN",
  };
  for (unsigned i = 0; i < 2; i++) {
    void *sensor = NULL;
    int rc = get_sensor(uris[i], &sensor);
    if (!rc)
      rc = create(sensor, &listeners[i]);
    if (!rc)
      rc = set_cb(listeners[i], 0, wrist_event, (void *)(uintptr_t)(i + 1));
    if (!rc)
      rc = set_option(listeners[i], 1);
    if (!rc)
      rc = start(listeners[i]);
    printf("BRIDGE_WRIST_%s_START=%d\n", i ? "DOWN" : "UP", rc);
    if (rc) {
      result = 5;
      goto cleanup;
    }
  }
  void *light_sensor = NULL;
  int light_rc = get_sensor(
      "http://tizen.org/sensor/general/light/SENSOR_LIGHT", &light_sensor);
  if (!light_rc)
    light_rc = create(light_sensor, &listeners[2]);
  if (!light_rc)
    light_rc = set_cb(listeners[2], 2000, light_event, NULL);
  if (!light_rc)
    light_rc = start(listeners[2]);
  printf("BRIDGE_LIGHT_START=%d\n", light_rc);
  s_has.wrist = true;
  s_has.light = light_rc == 0;
  fd = pbl_adapter_bind(s_dir, PBL_ADAPTER_ADAPTER_SOCK);
  if (fd < 0) {
    fprintf(stderr, "adapter socket in %s: %s\n", s_dir, strerror(-fd));
    result = fd == -EADDRINUSE ? 6 : 7;
    fd = -1;
    goto cleanup;
  }
  bound = true;
  s_health.fd = fd;
  signal(SIGTERM, stop_signal);
  signal(SIGINT, stop_signal);
  channel = io_new(fd);
  if (channel)
    io_id = io_watch(channel, 1, io_ready, NULL);
  void *aod_channel = s_aod.alarm >= 0 ? io_new(s_aod.alarm) : NULL;
  if (aod_channel)
    io_watch(aod_channel, 1, aod_alarm_ready, NULL);
  void *wake_channel = s_wake_alarm >= 0 ? io_new(s_wake_alarm) : NULL;
  if (wake_channel)
    io_watch(wake_channel, 1, wake_alarm_ready, NULL);
  printf("BRIDGE_AOD_LOWPOWER=%d alarm=%d bus=%d\n", lowpower, s_aod.alarm >= 0, s_aod.bus != NULL);
  // Interactive mode needs 1 s to time out the screen; idle only drives sensor recovery.
  unsigned tick_seconds = 1;
  tick_id = add_tick(tick_seconds, tick, NULL);
  if (!channel || !io_id || !tick_id) {
    result = 8;
    goto cleanup;
  }
  subscribe_lcd_signals();
  printf("BRIDGE_READY duration=%d ambient_clock=%d\n", seconds, ambient_clock);
  send_ready();
  int64_t deadline = monotonic_seconds() + seconds;
  bool light_running = light_rc == 0;
  // Our own display transitions echo back as LCD signals; ignore them for a moment after each.
  uint64_t transition_ms = 0;
  while (!stopped && monotonic_seconds() < deadline) {
    iterate(NULL, true);
    if (s_trace.fd >= 0 && trace_clock() >= s_trace.deadline)
      trace_stop("deadline");
    sig_atomic_t action = wrist_action;
    wrist_action = 0;
    if (action == 1) {
      // Fast path: light the panel now. The OS confirms with DISPLAY ON or takes it back.
      if (s_want.mode != PBL_ADAPTER_DISPLAY_ON) {
        s_want.mode = PBL_ADAPTER_DISPLAY_ON;
        s_want.seq = 0;
      }
      send_gesture(PBL_ADAPTER_GESTURE_WRIST_UP);
      printf("BRIDGE_WRIST_UP t=%llu age_ms=%lld\n", (unsigned long long)boot_ms(),
             wrist_age_ms());
    } else if (action == 2) {
      send_gesture(PBL_ADAPTER_GESTURE_WRIST_DOWN);
      printf("BRIDGE_WRIST_DOWN t=%llu age_ms=%lld\n", (unsigned long long)boot_ms(),
             wrist_age_ms());
    }
    sig_atomic_t lcd = lcd_signal;
    lcd_signal = 0;
    if (lcd && display_initialized && boot_ms() - transition_ms > 1500) {
      if (lcd == 1 && previous_ambient) {
        s_want.mode = PBL_ADAPTER_DISPLAY_ON;
        s_want.seq = 0;
        send_gesture(PBL_ADAPTER_GESTURE_EXTERNAL_WAKE);
        printf("BRIDGE_LCD_ON_WAKE t=%llu\n", (unsigned long long)boot_ms());
      } else if (lcd == 2 && !previous_ambient) {
        // The vendor stack blanked the panel; the one case where the adapter turns it off itself.
        s_want.mode = PBL_ADAPTER_DISPLAY_OFF;
        s_want.seq = 0;
        send_gesture(PBL_ADAPTER_GESTURE_EXTERNAL_OFF);
        printf("BRIDGE_LCD_OFF_IDLE t=%llu\n", (unsigned long long)boot_ms());
      }
    }
    uint8_t message[128];
    ssize_t count;
    for (unsigned received = 0;
         received < 32 && (count = recv(fd, message, sizeof(message), MSG_DONTWAIT)) >= 0;
         received++) {
      struct pbl_adapter_hdr hdr;
      if (!pbl_adapter_accept(message, (size_t)count, &hdr))
        continue;
      switch (hdr.type) {
        case PBL_ADAPTER_HELLO:
          send_caps(hdr.seq);
          publish_state();
          printf("BRIDGE_HELLO seq=%u\n", hdr.seq);
          break;
        case PBL_ADAPTER_BACKLIGHT: {
          struct pbl_adapter_backlight backlight;
          memcpy(&backlight, message, sizeof(backlight));
          if (backlight.level <= 100)
            brightness = backlight.level;
          break;
        }
        case PBL_ADAPTER_DISPLAY: {
          struct pbl_adapter_display display;
          memcpy(&display, message, sizeof(display));
          if (display.mode >= PBL_ADAPTER_DISPLAY_ON &&
              display.mode <= PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK) {
            s_want.mode = display.mode;
            s_want.seq = hdr.seq;
          }
          break;
        }
        case PBL_ADAPTER_USER_ACTIVITY:
          // Fast path: the OS follows with DISPLAY ON, or does not.
          if (s_want.mode != PBL_ADAPTER_DISPLAY_ON) {
            s_want.mode = PBL_ADAPTER_DISPLAY_ON;
            s_want.seq = 0;
          }
          break;
        case PBL_ADAPTER_ACCEL_CONFIG: {
          struct pbl_adapter_accel_config accel;
          memcpy(&accel, message, sizeof(accel));
          bool valid = accel.samples <= PBL_ADAPTER_SENSOR_SAMPLES &&
                       (!accel.samples || (accel.interval_us >= 10000 &&
                                           accel.interval_us <= 1000000));
          if (valid) {
            s_health.requested_samples = accel.samples;
            s_health.requested_interval_ms = accel.samples ? accel.interval_us / 1000 : 40;
            update_accel_rate(previous_ambient, true);
          }
          break;
        }
        case PBL_ADAPTER_HRM_CONFIG: {
          struct pbl_adapter_hrm_config hrm;
          memcpy(&hrm, message, sizeof(hrm));
          if (hrm.mode <= PBL_ADAPTER_HRM_BPM)
            configure_health(1, hrm.mode == PBL_ADAPTER_HRM_BPM, 1000);
          break;
        }
        case PBL_ADAPTER_IDLE: {
          struct pbl_adapter_idle idle_message;
          memcpy(&idle_message, message, sizeof(idle_message));
          if (!s_has.suspend)
            break;
          // The lock expires by itself, so an OS that dies busy cannot hold the watch awake.
          if (idle_message.busy) {
            request_lock(0 /* CPU */, 3000);
            s_busy_lock = true;
            arm_wake_alarm(0);
          } else {
            if (s_busy_lock)
              release_lock(0);
            s_busy_lock = false;
            arm_wake_alarm(idle_message.next_deadline_us);
          }
          break;
        }
        case PBL_ADAPTER_TRACE: {
          struct pbl_adapter_trace trace;
          memcpy(&trace, message, sizeof(trace));
          if (trace.enable)
            printf("SENSOR_TRACE_RESULT=%d\n", trace_start(s_trace_path, trace.seconds));
          else
            trace_stop("requested");
          break;
        }
        case PBL_ADAPTER_VIBE: {
          struct pbl_adapter_vibe vibe;
          memcpy(&vibe, message, sizeof(vibe));
          bool stop = vibe.strength == 0 || vibe.duration_ms == 0;
          // An expired pulse is dropped: the adapter may have stalled or the system suspended.
          if (!stop &&
              !pbl_adapter_vibe_remaining_ms(&vibe, boot_ms(), PBL_ADAPTER_VIBE_MAX_MS))
            break;
          if (active) {
            if (boot_ms() < haptic_deadline_ms)
              stop_haptic(haptic, effect);
            active = false;
            effect = NULL;
          }
          if (stop) {
            printf("BRIDGE_VIBRATE_STOP\n");
            break;
          }
          // A synchronous stop can outlast a short pulse's remaining lifetime.
          unsigned duration =
              pbl_adapter_vibe_remaining_ms(&vibe, boot_ms(), PBL_ADAPTER_VIBE_MAX_MS);
          if (!duration)
            break;
          int rc = vibrate(haptic, duration, vibe.strength, &effect);
          active = rc == 0;
          haptic_deadline_ms = boot_ms() + duration;
          printf("BRIDGE_VIBRATE strength=%d duration=%u result=%d\n", vibe.strength, duration,
                 rc);
          break;
        }
        default:
          break;
      }
    }
    if (s_aod.minute_due) {
      s_aod.minute_due = 0;
      if (aod_active) {
        // Stay awake while the renderer draws the new minute, then hand the panel back.
        request_lock(0 /* CPU */, 2000);
        aod_signal("clockstart");
        publish_state();
        add_timeout(CLOCKEND_DELAY_MS, aod_clockend_timer, NULL);
        aod_arm_minute(true);
      }
    }
    if (s_aod.clockend_due) {
      s_aod.clockend_due = 0;
      if (aod_active)
        aod_signal("clockend");
    }
    // The OS owns the display timeout. If it is gone, do not hold the panel on for it.
    if (s_os_alive) {
      os_gone_since = 0;
    } else if (!os_gone_since) {
      os_gone_since = monotonic_seconds();
    } else if (monotonic_seconds() - os_gone_since >= OS_GONE_GRACE_S &&
               s_want.mode == PBL_ADAPTER_DISPLAY_ON) {
      s_want.mode = PBL_ADAPTER_DISPLAY_OFF;
      s_want.seq = 0;
      printf("BRIDGE_OS_GONE display off\n");
    }
    ambient = s_want.mode != PBL_ADAPTER_DISPLAY_ON;
    ambient_clock = s_want.mode == PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK;
    clock_changed = ambient && ambient_clock != previous_clock;
    poll_steps(trace_clock(), ambient);
    update_accel_rate(ambient, false);
    if (listeners[2] && light_rc == 0 && light_running == ambient) {
      int rc = ambient ? stop(listeners[2]) : start(listeners[2]);
      if (!rc)
        light_running = !ambient;
      printf("BRIDGE_LIGHT_SAMPLING=%d result=%d\n", !ambient, rc);
    }
    if (ambient != previous_ambient || !display_initialized || clock_changed) {
      clock_changed = false;
      transition_ms = boot_ms();
      bool use_aod = ambient && ambient_clock && lowpower && s_aod.bus && add_timeout;
      int display_rc;
      if (aod_active && !use_aod) {
        aod_arm_minute(false);
        aod_active = false;
      }
      if (use_aod) {
        // deviced only enters its low-power (ALPM) display state for a visible Samsung watch face;
        // starter resets this runtime flag at every screen-on.
        int visible = s_aod.vconf_set_int
            ? s_aod.vconf_set_int("memory/homescreen/watchface_visibility", 1) : -ENOSYS;
        printf("BRIDGE_AOD_VISIBILITY=%d\n", visible);
        // Leave the dim clock brightness; deviced keeps the panel lit in ambient mode.
        display_rc = apply_display_power(s_want.mode, true, &display_ops);
        printf("BRIDGE_AMBIENT lowpower off=%d\n", display_rc);
        if (!display_rc) {
          add_timeout(CLOCKEND_DELAY_MS, aod_clockend_timer, NULL);
          aod_arm_minute(true);
          aod_active = true;
        }
      } else if (ambient && !ambient_clock) {
        // deviced keeps the panel lit, dimmed, for about 5 s after an off request. Blank the
        // backlight first so the screen goes black at once; wake restores the brightness.
        int blank = write_brightness(0);
        if (blank)
          blank = write_brightness(1);
        applied_brightness = 0;
        display_rc = apply_display_power(s_want.mode, false, &display_ops);
        printf("BRIDGE_AMBIENT off=%d blank=%d\n", display_rc, blank);
      } else {
        display_rc = apply_display_power(s_want.mode, false, &display_ops);
        last_panel_recovery = monotonic_seconds() - 9;
        // Sparse ambient clock uses the normal compositor until hardware
        // doze can restore the interactive display reliably.
        printf("BRIDGE_DISPLAY_LOCK=%d ambient=%d t=%llu\n", display_rc,
               ambient, (unsigned long long)boot_ms());
      }
      if (!display_rc) {
        previous_ambient = ambient;
        previous_clock = ambient_clock;
        display_initialized = true;
      }
    }
    // The panel now reflects the latest DISPLAY message.
    if (s_display_actual == s_want.mode && !s_display_failed)
      s_want.applied_seq = s_want.seq;
    unsigned target = ambient ? 2 : brightness;
    if ((!ambient || ambient_clock) && applied_brightness != (int)target) {
      int rc = write_brightness(target);
      applied_brightness = target;
      printf("BRIDGE_BRIGHTNESS=%u result=%d\n", target, rc);
    }
    unsigned wanted_tick = ambient ? 10 : 1;
    if (wanted_tick != tick_seconds) {
      unsigned replacement = add_tick(wanted_tick, tick, NULL);
      if (replacement) {
        remove_source(tick_id);
        tick_id = replacement;
        tick_seconds = wanted_tick;
      }
    }
    int64_t now = monotonic_seconds();
    if (!ambient && now - last_panel_recovery >= 10 && panel_power() > 0) {
      last_panel_recovery = now;
      recover_panel_if_off();
    }
    if (last_publish == 0 || now - last_publish >= 10 ||
        ambient != previous_ambient || published_lux != light_lux ||
        published_ack != s_want.applied_seq) {
      publish_state();
      last_publish = now;
      published_lux = light_lux;
      published_ack = s_want.applied_seq;
    }
    // Publish transitions immediately even when the lux reading is unchanged.
    if (s_policy_dirty) {
      publish_state();
      s_policy_dirty = false;
    }
    static int published_ambient = -1;
    if (published_ambient != (int)ambient) {
      publish_state();
      published_ambient = ambient;
      printf("BRIDGE_MODE=%s lux=%u valid=%d\n",
             ambient ? "ambient" : "interactive", light_lux, light_valid);
    }
  }
cleanup:
  trace_stop("adapter_exit");
  if (s_steps.listener)
    destroy(s_steps.listener);
  for (unsigned i=0;i<2;i++) {
    if (s_health.listener[i]) {
      stop(s_health.listener[i]);
      destroy(s_health.listener[i]);
    }
  }
  if (io_id)
    remove_source(io_id);
  if (s_aod.alarm >= 0)
    close(s_aod.alarm);
  if (tick_id)
    remove_source(tick_id);
  if (channel)
    io_unref(channel);
  if (active)
    stop_haptic(haptic, effect);
  if (s_busy_lock)
    release_lock(0);
  if (s_wake_alarm >= 0)
    close(s_wake_alarm);
  if (fd >= 0)
    close(fd);
  if (bound) {
    char path[256];
    if (!pbl_adapter_join(path, sizeof(path), s_dir, PBL_ADAPTER_ADAPTER_SOCK))
      unlink(path);
  }
  for (unsigned i = 0; i < 3; i++) {
    if (listeners[i]) {
      stop(listeners[i]);
      destroy(listeners[i]);
    }
  }
  close_haptic(haptic);
  if (bound) {
    unlink(s_policy_path);
    char stale[sizeof(s_policy_path) + 8];
    snprintf(stale, sizeof(stale), "%s.new", s_policy_path);
    unlink(stale);
    write_brightness(original_brightness);
    release_lock(1);
    wake(false);
    recover_panel_if_off();
  }
  printf("BRIDGE_STOPPED result=%d\n", result);
  return result;
}
