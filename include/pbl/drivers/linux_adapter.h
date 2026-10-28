/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * PebbleOS Linux device-adapter protocol, version 2.
 *
 * This header is the whole wire contract between the OS and a device adapter. An adapter takes
 * this one file from the OS tree and nothing else. It builds for 32 or 64-bit little-endian
 * targets, and on macOS for host tests. See spec/adapter-protocol.md for the behavior around it.
 *
 * Every datagram starts with struct pbl_adapter_hdr. The OS binds os.sock and the adapter binds
 * adapter.sock, both under the runtime directory.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The adapter protocol is little-endian"
#endif

#define PBL_ADAPTER_MAGIC 0x5042u /* 'PB' */
#define PBL_ADAPTER_MAJOR 2
#define PBL_ADAPTER_MINOR 0

#define PBL_ADAPTER_ADAPTER_SOCK "adapter.sock" /* bound by the adapter, OS sends here */
#define PBL_ADAPTER_OS_SOCK      "os.sock"      /* bound by the OS, adapter sends here */

/* Longest socket path that fits sockaddr_un on every supported host (macOS has 104 bytes). */
#define PBL_ADAPTER_PATH_MAX 104

/* The adapter must send STATE at least this often; the OS gives up after three missed periods. */
#define PBL_ADAPTER_HEARTBEAT_S 60

/* Types 0x0000-0x7FFF are core. 0x8000-0xFFFF are debug or vendor, and may be ignored. */
enum pbl_adapter_type {
  /* OS to adapter */
  PBL_ADAPTER_HELLO = 0x0001,
  PBL_ADAPTER_VIBE = 0x0002,
  PBL_ADAPTER_BACKLIGHT = 0x0003,
  PBL_ADAPTER_DISPLAY = 0x0004,
  PBL_ADAPTER_USER_ACTIVITY = 0x0005,
  PBL_ADAPTER_ACCEL_CONFIG = 0x0006,
  PBL_ADAPTER_HRM_CONFIG = 0x0007,
  PBL_ADAPTER_IDLE = 0x0008,
  /* adapter to OS */
  PBL_ADAPTER_ADAPTER_READY = 0x0101,
  PBL_ADAPTER_CAPS = 0x0102,
  PBL_ADAPTER_STATE = 0x0103,
  PBL_ADAPTER_SENSOR = 0x0104,
  PBL_ADAPTER_GESTURE = 0x0105,
  PBL_ADAPTER_FAULT = 0x0106,
  /* debug range */
  PBL_ADAPTER_TRACE = 0x8001,
};

enum pbl_adapter_cap {
  PBL_ADAPTER_CAP_VIBE = 1u << 0,
  PBL_ADAPTER_CAP_BACKLIGHT = 1u << 1,
  PBL_ADAPTER_CAP_ALS = 1u << 2,
  PBL_ADAPTER_CAP_ACCEL = 1u << 3,
  PBL_ADAPTER_CAP_HRM = 1u << 4,
  PBL_ADAPTER_CAP_STEPS = 1u << 5,
  PBL_ADAPTER_CAP_WRIST_GESTURE = 1u << 6,
  PBL_ADAPTER_CAP_DISPLAY_POWER = 1u << 7,
  PBL_ADAPTER_CAP_AMBIENT_LOWPOWER = 1u << 8,
  PBL_ADAPTER_CAP_SUSPEND = 1u << 9,
};

enum pbl_adapter_display_mode {
  PBL_ADAPTER_DISPLAY_ON = 1,
  PBL_ADAPTER_DISPLAY_OFF = 2,
  PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK = 3,
};

enum pbl_adapter_display_reason {
  PBL_ADAPTER_REASON_TIMEOUT = 1,
  PBL_ADAPTER_REASON_USER = 2,
  PBL_ADAPTER_REASON_PALM = 3,
  PBL_ADAPTER_REASON_GESTURE = 4,
  PBL_ADAPTER_REASON_APP = 5,
};

enum pbl_adapter_activity_source {
  PBL_ADAPTER_ACTIVITY_BUTTON = 1,
  PBL_ADAPTER_ACTIVITY_BEZEL = 2,
  PBL_ADAPTER_ACTIVITY_TOUCH = 3,
};

enum pbl_adapter_hrm_mode {
  PBL_ADAPTER_HRM_OFF = 0,
  PBL_ADAPTER_HRM_BPM = 1,
};

enum pbl_adapter_gesture_kind {
  PBL_ADAPTER_GESTURE_WRIST_UP = 1,
  PBL_ADAPTER_GESTURE_WRIST_DOWN = 2,
  PBL_ADAPTER_GESTURE_PALM_COVER = 3,
  PBL_ADAPTER_GESTURE_EXTERNAL_WAKE = 4, /* vendor stack lit the panel (Samsung LCDOn) */
  PBL_ADAPTER_GESTURE_EXTERNAL_OFF = 5,  /* vendor stack blanked the panel (Samsung LCDOff) */
};

enum pbl_adapter_component {
  PBL_ADAPTER_COMPONENT_ACCEL = 1,
  PBL_ADAPTER_COMPONENT_HRM = 2,
  PBL_ADAPTER_COMPONENT_STEPS = 3,
  PBL_ADAPTER_COMPONENT_DISPLAY = 4,
  PBL_ADAPTER_COMPONENT_ALS = 5,
  PBL_ADAPTER_COMPONENT_VIBE = 6,
};

enum pbl_adapter_fault_state {
  PBL_ADAPTER_FAULT_OK = 0,
  PBL_ADAPTER_FAULT_RECOVERING = 1,
  PBL_ADAPTER_FAULT_FAILED = 2,
};

enum pbl_adapter_sensor_type {
  PBL_ADAPTER_SENSOR_ACCEL = 1,
  PBL_ADAPTER_SENSOR_HRM = 2,
  PBL_ADAPTER_SENSOR_STEPS = 3,
};

#define PBL_ADAPTER_SENSOR_SAMPLES        125
#define PBL_ADAPTER_ACCEL_MIN_INTERVAL_US 1000u
#define PBL_ADAPTER_ACCEL_MAX_INTERVAL_US 1000000u
#define PBL_ADAPTER_VIBE_MAX_MS           5000u

/* Envelope, 12 bytes. */
struct pbl_adapter_hdr {
  uint16_t magic;
  uint8_t major;
  uint8_t minor; /* additive revisions within a major version */
  uint16_t type;
  uint16_t length; /* total datagram bytes, header included */
  uint32_t seq;    /* sender's counter */
};

/* OS to adapter. */

struct pbl_adapter_hello {
  struct pbl_adapter_hdr hdr;
  char os_build[16];
  uint8_t max_minor;
  uint8_t reserved[3];
};

struct pbl_adapter_vibe {
  struct pbl_adapter_hdr hdr;
  uint8_t strength; /* 1-100; 0 stops */
  uint8_t reserved;
  uint16_t duration_ms; /* 0 stops */
  /* CLOCK_BOOTTIME ms after which the command is stale. An adapter that was stalled or the whole
   * system suspended must not replay an expired pulse. */
  uint64_t deadline_ms;
};

struct pbl_adapter_backlight {
  struct pbl_adapter_hdr hdr;
  uint8_t level; /* 0-100, brightness only; never changes display power */
  uint8_t reserved[3];
};

struct pbl_adapter_display {
  struct pbl_adapter_hdr hdr;
  uint8_t mode;   /* enum pbl_adapter_display_mode */
  uint8_t reason; /* enum pbl_adapter_display_reason */
  uint8_t reserved[2];
};

struct pbl_adapter_user_activity {
  struct pbl_adapter_hdr hdr;
  uint8_t source; /* enum pbl_adapter_activity_source */
  uint8_t reserved[3];
};

struct pbl_adapter_accel_config {
  struct pbl_adapter_hdr hdr;
  uint16_t samples; /* per packet, 0 stops */
  uint16_t reserved;
  uint32_t interval_us;
};

struct pbl_adapter_hrm_config {
  struct pbl_adapter_hdr hdr;
  uint8_t mode; /* enum pbl_adapter_hrm_mode */
  uint8_t low_latency;
  uint8_t reserved[2];
};

struct pbl_adapter_idle {
  struct pbl_adapter_hdr hdr;
  uint8_t busy;
  uint8_t reserved[3];
  uint64_t next_deadline_us; /* CLOCK_BOOTTIME, 0 = none */
};

struct pbl_adapter_trace {
  struct pbl_adapter_hdr hdr;
  uint8_t enable;
  uint8_t reserved;
  uint16_t seconds; /* 1-300 */
};

/* Adapter to OS. ADAPTER_READY is a bare header. */

struct pbl_adapter_caps {
  struct pbl_adapter_hdr hdr;
  uint32_t ack_seq; /* the HELLO this answers; 0 if unprompted */
  uint32_t caps;    /* enum pbl_adapter_cap bits */
  uint32_t accel_min_interval_us;
  uint32_t accel_max_interval_us;
  uint16_t accel_max_samples;
  uint16_t vibe_max_duration_ms;
  char adapter_name[16];
  char adapter_version[16];
};

struct pbl_adapter_state {
  struct pbl_adapter_hdr hdr;
  uint32_t ack_seq;       /* seq of the DISPLAY message this reflects; 0 if none yet */
  uint8_t display_actual; /* enum pbl_adapter_display_mode: what the panel is doing now */
  uint8_t lux_valid;
  uint16_t lux;
};

struct pbl_adapter_gesture {
  struct pbl_adapter_hdr hdr;
  uint8_t kind; /* enum pbl_adapter_gesture_kind */
  uint8_t reserved[3];
  uint64_t timestamp_us; /* CLOCK_BOOTTIME */
};

struct pbl_adapter_fault {
  struct pbl_adapter_hdr hdr;
  uint8_t component; /* enum pbl_adapter_component */
  uint8_t state;     /* enum pbl_adapter_fault_state */
  uint8_t reserved[2];
};

/*
 * Sensor batch. Every value is already in Pebble units and axis orientation.
 *
 *  ACCEL: `count` rows of [x, y, z] milli-g, clamped to +-32767, evenly spaced at interval_us.
 *  HRM:   one row, [bpm, quality]. quality is a Pebble HRMQuality value, -1 for off-wrist.
 *  STEPS: one row, [new steps since the last packet (<= 30000), elapsed seconds].
 */
struct pbl_adapter_sensor {
  struct pbl_adapter_hdr hdr;
  uint32_t type; /* enum pbl_adapter_sensor_type */
  uint32_t count;
  uint32_t interval_us;
  uint32_t reserved[2];
  uint64_t timestamp_us; /* first row, CLOCK_BOOTTIME */
  int16_t values[PBL_ADAPTER_SENSOR_SAMPLES][3];
};

#define PBL_ADAPTER_ASSERT_SIZE(type, size) \
  _Static_assert(sizeof(struct type) == (size), "adapter protocol: " #type " size")

_Static_assert(sizeof(struct pbl_adapter_hdr) == 12, "adapter protocol: header size");
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_hello, 32);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_vibe, 24);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_backlight, 16);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_display, 16);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_user_activity, 16);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_accel_config, 20);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_hrm_config, 16);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_idle, 24);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_trace, 16);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_caps, 64);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_state, 20);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_gesture, 24);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_fault, 16);
PBL_ADAPTER_ASSERT_SIZE(pbl_adapter_sensor, 792);
_Static_assert(offsetof(struct pbl_adapter_vibe, deadline_ms) == 16, "adapter protocol: vibe");
_Static_assert(offsetof(struct pbl_adapter_idle, next_deadline_us) == 16, "adapter protocol: idle");
_Static_assert(offsetof(struct pbl_adapter_gesture, timestamp_us) == 16,
               "adapter protocol: gesture");
_Static_assert(offsetof(struct pbl_adapter_sensor, timestamp_us) == 32, "adapter protocol: sensor");
_Static_assert(offsetof(struct pbl_adapter_sensor, values) == 40, "adapter protocol: sensor");

/* Smallest valid length of a type, or 0 for a type this version does not know. */
static inline size_t pbl_adapter_min_length(uint16_t type) {
  switch (type) {
    case PBL_ADAPTER_HELLO:
      return sizeof(struct pbl_adapter_hello);
    case PBL_ADAPTER_VIBE:
      return sizeof(struct pbl_adapter_vibe);
    case PBL_ADAPTER_BACKLIGHT:
      return sizeof(struct pbl_adapter_backlight);
    case PBL_ADAPTER_DISPLAY:
      return sizeof(struct pbl_adapter_display);
    case PBL_ADAPTER_USER_ACTIVITY:
      return sizeof(struct pbl_adapter_user_activity);
    case PBL_ADAPTER_ACCEL_CONFIG:
      return sizeof(struct pbl_adapter_accel_config);
    case PBL_ADAPTER_HRM_CONFIG:
      return sizeof(struct pbl_adapter_hrm_config);
    case PBL_ADAPTER_IDLE:
      return sizeof(struct pbl_adapter_idle);
    case PBL_ADAPTER_ADAPTER_READY:
      return sizeof(struct pbl_adapter_hdr);
    case PBL_ADAPTER_CAPS:
      return sizeof(struct pbl_adapter_caps);
    case PBL_ADAPTER_STATE:
      return sizeof(struct pbl_adapter_state);
    case PBL_ADAPTER_SENSOR:
      return sizeof(struct pbl_adapter_sensor);
    case PBL_ADAPTER_GESTURE:
      return sizeof(struct pbl_adapter_gesture);
    case PBL_ADAPTER_FAULT:
      return sizeof(struct pbl_adapter_fault);
    case PBL_ADAPTER_TRACE:
      return sizeof(struct pbl_adapter_trace);
    default:
      return 0;
  }
}

/*
 * Zeroes a message and writes its header. `size` is the full struct size.
 * Reserved bytes are always zero on the wire.
 */
static inline void pbl_adapter_init(void *message, uint16_t type, size_t size, uint32_t seq) {
  struct pbl_adapter_hdr hdr = {
    .magic = PBL_ADAPTER_MAGIC,
    .major = PBL_ADAPTER_MAJOR,
    .minor = PBL_ADAPTER_MINOR,
    .type = type,
    .length = (uint16_t)size,
    .seq = seq,
  };
  memset(message, 0, size);
  memcpy(message, &hdr, sizeof(hdr));
}

/*
 * Checks a received datagram. Returns 1 and fills `hdr` when it is one this version understands:
 * right magic and major, a known type, and at least the type's minimum length. Anything else
 * returns 0 and the receiver drops it. Bytes past hdr->length are ignored, so a newer minor
 * version can append fields.
 */
static inline int pbl_adapter_accept(const void *data, size_t received,
                                     struct pbl_adapter_hdr *hdr) {
  if (received < sizeof(*hdr))
    return 0;
  memcpy(hdr, data, sizeof(*hdr));
  size_t minimum = pbl_adapter_min_length(hdr->type);
  if (hdr->magic != PBL_ADAPTER_MAGIC || hdr->major != PBL_ADAPTER_MAJOR || !minimum ||
      hdr->length < minimum || hdr->length > received)
    return 0;
  return 1;
}

/* Copies a string into a fixed field, always terminated. */
static inline void pbl_adapter_copy_name(char *field, size_t size, const char *value) {
  memset(field, 0, size);
  if (value)
    strncpy(field, value, size - 1);
}

static inline uint64_t pbl_adapter_boottime_us(void) {
  struct timespec now;
#ifdef CLOCK_BOOTTIME
  clock_gettime(CLOCK_BOOTTIME, &now);
#else
  clock_gettime(CLOCK_MONOTONIC, &now);
#endif
  return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/*
 * Fills a VIBE message that expires `duration_ms` from now. Strength and duration of 0 build a
 * stop command.
 */
static inline void pbl_adapter_build_vibe(struct pbl_adapter_vibe *message, uint32_t seq,
                                          unsigned strength, unsigned duration_ms) {
  pbl_adapter_init(message, PBL_ADAPTER_VIBE, sizeof(*message), seq);
  if (!strength || !duration_ms)
    return;
  message->strength = strength > 100 ? 100 : (uint8_t)strength;
  message->duration_ms =
      duration_ms > PBL_ADAPTER_VIBE_MAX_MS ? PBL_ADAPTER_VIBE_MAX_MS : (uint16_t)duration_ms;
  message->deadline_ms = pbl_adapter_boottime_us() / 1000u + message->duration_ms;
}

/*
 * Milliseconds left to vibrate, or 0 for a stop command, a malformed one, or one that has expired.
 * `max_ms` is the adapter's declared vibe_max_duration_ms.
 */
static inline unsigned pbl_adapter_vibe_remaining_ms(const struct pbl_adapter_vibe *message,
                                                     uint64_t now_ms, unsigned max_ms) {
  if (message->strength == 0 || message->strength > 100 || message->duration_ms == 0 ||
      message->duration_ms > max_ms || message->deadline_ms <= now_ms)
    return 0;
  uint64_t remaining = message->deadline_ms - now_ms;
  return remaining <= message->duration_ms ? (unsigned)remaining : 0;
}

/*
 * Runtime directory, in priority order: an explicit override (the --runtime-dir option),
 * PBL_RUNTIME_DIR, $XDG_RUNTIME_DIR/pebble, then the device profile's default.
 * Returns 0, or -ENAMETOOLONG / -ENOENT.
 */
static inline int pbl_adapter_runtime_dir(char *out, size_t size, const char *override_dir,
                                          const char *profile_default) {
  const char *env = getenv("PBL_RUNTIME_DIR");
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  int n;
  if (override_dir && *override_dir)
    n = snprintf(out, size, "%s", override_dir);
  else if (env && *env)
    n = snprintf(out, size, "%s", env);
  else if (xdg && *xdg)
    n = snprintf(out, size, "%s/pebble", xdg);
  else if (profile_default && *profile_default)
    n = snprintf(out, size, "%s", profile_default);
  else
    return -ENOENT;
  return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

/* Joins a directory and file name. Socket paths must also fit PBL_ADAPTER_PATH_MAX. */
static inline int pbl_adapter_join(char *out, size_t size, const char *dir, const char *name) {
  int n = snprintf(out, size, "%s/%s", dir, name);
  return n < 0 || (size_t)n >= size ? -ENAMETOOLONG : 0;
}

static inline int pbl_adapter_socket_address(struct sockaddr_un *address, const char *dir,
                                             const char *name) {
  char path[256];
  int rc = pbl_adapter_join(path, sizeof(path), dir, name);
  if (rc)
    return rc;
  if (strlen(path) >= PBL_ADAPTER_PATH_MAX || strlen(path) >= sizeof(address->sun_path))
    return -ENAMETOOLONG;
  memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  strcpy(address->sun_path, path);
  return 0;
}

static inline int pbl_adapter_set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    return -errno;
  return 0;
}

/*
 * Binds a non-blocking datagram socket at dir/name with mode 0600. A stale socket file from a
 * crashed process is replaced, but a live endpoint is not: the file must be a socket owned by the
 * same uid and connect() must fail with ECONNREFUSED. Returns the descriptor or -errno
 * (-EADDRINUSE for a live endpoint).
 */
static inline int pbl_adapter_bind(const char *dir, const char *name) {
  struct sockaddr_un address;
  int rc = pbl_adapter_socket_address(&address, dir, name);
  if (rc)
    return rc;
  struct stat previous;
  if (!lstat(address.sun_path, &previous)) {
    if (!S_ISSOCK(previous.st_mode) || previous.st_uid != geteuid())
      return -EADDRINUSE;
    int probe = socket(AF_UNIX, SOCK_DGRAM, 0);
    int connected = probe >= 0 ? connect(probe, (struct sockaddr *)&address, sizeof(address)) : -1;
    int error = errno;
    if (probe >= 0)
      close(probe);
    if (!connected || error != ECONNREFUSED)
      return -EADDRINUSE;
    unlink(address.sun_path);
  }
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0)
    return -errno;
  rc = pbl_adapter_set_nonblock(fd);
  if (!rc && bind(fd, (struct sockaddr *)&address, sizeof(address)))
    rc = -errno;
  if (rc) {
    close(fd);
    return rc;
  }
  if (chmod(address.sun_path, 0600)) {
    rc = -errno;
    close(fd);
    unlink(address.sun_path);
    return rc;
  }
  return fd;
}

/* Sends one datagram without blocking. Returns 0 or -errno. */
static inline int pbl_adapter_send(int fd, const struct sockaddr_un *peer, const void *message,
                                   size_t size) {
  if (fd < 0)
    return -ENOTCONN;
  ssize_t sent =
      sendto(fd, message, size, MSG_DONTWAIT, (const struct sockaddr *)peer, sizeof(*peer));
  return sent == (ssize_t)size ? 0 : -errno;
}

#undef PBL_ADAPTER_ASSERT_SIZE
