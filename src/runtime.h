#ifndef ADM_RUNTIME_H
#define ADM_RUNTIME_H
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
static inline uint64_t adm_milliseconds(void) { return GetTickCount64(); }
#else
#include <time.h>
static inline uint64_t adm_milliseconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
#endif
#endif
