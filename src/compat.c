/* compat.c - glibc < 2.38 C23 symbol shims.
 *
 * Newer glibc headers map strtol/sscanf to __isoc23_* when built with
 * _GNU_SOURCE (C23 semantics); old distro libcs (e.g. ubuntu 22.04,
 * glibc 2.35) do not carry those symbols, so linking fails. We define
 * them here with identical ISO C23 behavior; on newer glibcs these
 * strong definitions simply shadow the dynamic-library versions.
 *
 * NOTE: _GNU_SOURCE is undefined before the includes so these
 * functions call the plain symbols, not the mapped ones.
 */
#undef _GNU_SOURCE
#undef _DEFAULT_SOURCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

long __isoc23_strtol(const char *restrict nptr, char **restrict endptr,
                     int base) {
  return strtol(nptr, endptr, base);
}

int __isoc23_sscanf(const char *restrict s, const char *restrict format, ...) {
  va_list ap;
  va_start(ap, format);
  int r = vsscanf(s, format, ap);
  va_end(ap);
  return r;
}