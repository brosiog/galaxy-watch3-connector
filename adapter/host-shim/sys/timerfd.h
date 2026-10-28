#pragma once
#include <time.h>
#include <stdint.h>
#define TFD_NONBLOCK 1
#define TFD_CLOEXEC 2
#define TFD_TIMER_ABSTIME 1
struct itimerspec { struct timespec it_interval, it_value; };
int timerfd_create(int, int);
int timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *);
