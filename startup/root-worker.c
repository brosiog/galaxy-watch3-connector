#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <sys/mount.h>
#include <unistd.h>

#ifndef PEBBLE_STARTUP_SCRIPT
#define PEBBLE_STARTUP_SCRIPT "/opt/usr/home/owner/share/pebble-launcher/start-pebble"
#endif

// Enter the fixed launcher before the stock helper's main; discard the overlay.
// No caller-supplied command, argument or environment reaches the root script.
__attribute__((constructor)) static void launch(void) {
  if (setgroups(0, NULL) || setgid(0) || setuid(0) ||
      umount2("/lib/libcap.so.2", MNT_DETACH)) {
    dprintf(2, "pebble root worker: errno=%d\n", errno);
    _exit(1);
  }
  char *args[] = {"/bin/sh", PEBBLE_STARTUP_SCRIPT, NULL};
  char *env[] = {"PATH=/usr/bin:/usr/sbin:/bin:/sbin", "LC_ALL=C", NULL};
  execve(args[0], args, env);
  dprintf(2, "pebble launcher exec: errno=%d\n", errno);
  _exit(1);
}
