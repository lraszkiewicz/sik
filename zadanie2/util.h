#ifndef ZADANIE2_ERR_H_H
#define ZADANIE2_ERR_H_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>


// Zwraca aktualny czas w mikrosekundach.
uint64_t getCurrentTime() {
  timeval tv;
  gettimeofday(&tv, NULL);
  return ((uint64_t) tv.tv_sec) * 1'000'000 + ((uint64_t) tv.tv_usec);
}

// Wypisuje informację o błędnym zakończeniu funkcji systemowej
// i kończy działanie programu.
void syserr(const char *fmt, ...) {
  va_list fmt_args;

  fprintf(stderr, "ERROR: ");
  va_start(fmt_args, fmt);
  vfprintf(stderr, fmt, fmt_args);
  va_end(fmt_args);
  fprintf(stderr, " (%d; %s)\n", errno, strerror(errno));
  exit(EXIT_FAILURE);
}

// Wypisuje informację o błędzie i kończy działanie programu.
void fatal(const char *fmt, ...) {
  va_list fmt_args;

  fprintf(stderr, "ERROR: ");
  va_start(fmt_args, fmt);
  vfprintf(stderr, fmt, fmt_args);
  va_end(fmt_args);
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);
}

// Sprawdza, czy polecenie zwróciło 0 i w przeciwnym wypadku wywołuje syserr.
void checkSysError(int ret, const char *fmt) {
  if (ret < 0)
    syserr(fmt);
}

uint32_t parseUInt32(char *str) {
  unsigned long long res = strtoull(str, NULL, 10);
  if (errno == ERANGE || res <= 0 || res > UINT32_MAX) {
    fprintf(stderr, "\"%s\" is not a positive 32-bit integer.\n", str);
    exit(EXIT_FAILURE);
  }
  return (uint32_t) res;
}

uint16_t parseUInt16(char *str) {
  uint32_t res = parseUInt32(str);
  if (res > UINT16_MAX) {
    fprintf(stderr, "\"%s\" is not a positive 16-bit integer.\n", str);
    exit(EXIT_FAILURE);
  }
  return (uint16_t) res;
}

#endif //ZADANIE2_ERR_H_H
