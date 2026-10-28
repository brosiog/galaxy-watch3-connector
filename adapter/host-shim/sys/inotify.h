#pragma once
#define IN_NONBLOCK 1
#define IN_CLOEXEC 2
#define IN_MOVED_TO 1
#define IN_CLOSE_WRITE 2
#define IN_DELETE 4
static inline int inotify_init1(int f) { (void)f; return -1; }
static inline int inotify_add_watch(int fd, const char *p, unsigned m) { (void)fd;(void)p;(void)m; return -1; }
