/* Stand-ins for the Linux timer API, so the adapter's unit tests build on macOS. They do nothing;
 * the tests replace the calls they care about. */
#include <sys/timerfd.h>

int timerfd_create(int clock, int flags) {
  (void)clock;
  (void)flags;
  return 99;
}

int timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old) {
  (void)fd;
  (void)flags;
  (void)value;
  (void)old;
  return 0;
}
