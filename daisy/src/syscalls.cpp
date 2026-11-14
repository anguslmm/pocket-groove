// newlib's system calls, which this firmware never uses (no stdio streams, no files
// through libc): defined here so the C library links without nosys's "not
// implemented" warnings, each failing as nosys's would. Output goes through the USB
// serial port (usb_serial.h), files through pg::FileSystem.
#include <sys/stat.h>

#include <cerrno>

extern "C" {
int _close(int) {
  errno = EBADF;
  return -1;
}
int _fstat(int, struct stat* st) {
  st->st_mode = S_IFCHR;
  return 0;
}
int _isatty(int) { return 1; }
int _lseek(int, int, int) {
  errno = ESPIPE;
  return -1;
}
int _read(int, char*, int) {
  errno = EBADF;
  return -1;
}
int _write(int, const char*, int len) { return len; }
}
