#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mount.h>
#include <unistd.h>

#ifndef PEBBLE_WORKER_PATH
#define PEBBLE_WORKER_PATH "/opt/usr/home/owner/share/pebble-launcher/root-worker.so"
#endif

// Samsung's dotnet host supplies mount/setgid capabilities inherited from the
// owner service manager. All library overlays stay in this private namespace.
__attribute__((constructor)) static void bootstrap(void) {
  if (setgid(81) || unshare(CLONE_NEWNS) ||
      mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
      mount(PEBBLE_WORKER_PATH, "/lib/libcap.so.2", NULL, MS_BIND, NULL)) {
    dprintf(2, "pebble bootstrap: errno=%d\n", errno);
    _exit(1);
  }
  char *args[] = {"/usr/lib/dbus/dbus-daemon-launch-helper", NULL};
  char *env[] = {"PATH=/usr/bin:/usr/sbin:/bin:/sbin", NULL};
  execve(args[0], args, env);
  dprintf(2, "pebble helper exec: errno=%d\n", errno);
  _exit(1);
}
