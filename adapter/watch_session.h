#ifndef WATCH_SESSION_H
#define WATCH_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#define WATCH_SESSION_MODES "zero=unlimited"

// Zero opts into an unlimited session; positive review runs remain bounded.
static inline bool watch_session_parse(const char *value, long *seconds) {
  if (!value || !*value)
    return false;
  unsigned long result = 0;
  for (const char *p = value; *p; p++) {
    if (*p < '0' || *p > '9')
      return false;
    result = result * 10 + (unsigned)(*p - '0');
    if (result > 86400)
      return false;
  }
  if (result && result < 30)
    return false;
  *seconds = (long)result;
  return true;
}

static inline bool watch_session_active(long seconds, uint64_t now, uint64_t deadline) {
  return seconds == 0 || now < deadline;
}

#endif
