#include "watch_session.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  long seconds = -1;
  assert(watch_session_parse("0", &seconds) && seconds == 0);
  assert(watch_session_parse("30", &seconds) && seconds == 30);
  assert(watch_session_parse("86400", &seconds) && seconds == 86400);
  assert(watch_session_parse("00030", &seconds) && seconds == 30);
  const char *invalid[] = {"", "-1", "+30", " 30", "30x", "1", "29", "86401",
                           "9999999999999999999999999999999999"};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    assert(!watch_session_parse(invalid[i], &seconds));
  assert(!watch_session_parse(NULL, &seconds));
  // Simulate the deadline boundary and long uptime without a wall-clock wait.
  assert(watch_session_active(86400, 86399, 86400));
  assert(!watch_session_active(86400, 86400, 86400));
  assert(!watch_session_active(86400, 86401, 86400));
  assert(watch_session_active(0, 86400, 0));
  assert(watch_session_active(0, 86401, 0));
  assert(watch_session_active(0, 365ULL * 86400, 0));
  assert(watch_session_active(0, UINT64_MAX, 0));
  assert(watch_session_active(30, 129, 130));
  assert(!watch_session_active(30, 130, 130));
  puts("SESSION_UNLIMITED_AND_BOUNDED_DEADLINES_OK");
}
