/* Minimal newlib syscall stubs - CubeMX did not emit syscalls.c for this
   configuration. Only what snprintf/newlib-nano needs to link. */
#include <sys/stat.h>
#include <errno.h>

extern int errno;
register char *stack_ptr __asm__("sp");

void *_sbrk(int incr)
{
  extern char end __asm__("end");
  static char *heap_end;
  char *prev;
  if (heap_end == 0) heap_end = &end;
  prev = heap_end;
  if (heap_end + incr > stack_ptr) { errno = ENOMEM; return (void *)-1; }
  heap_end += incr;
  return (void *)prev;
}

int _write(int file, char *ptr, int len) { (void)file; (void)ptr; return len; }
int _read(int file, char *ptr, int len)  { (void)file; (void)ptr; (void)len; return 0; }
int _close(int file)                     { (void)file; return -1; }
int _fstat(int file, struct stat *st)    { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)                    { (void)file; return 1; }
int _lseek(int file, int ptr, int dir)   { (void)file; (void)ptr; (void)dir; return 0; }
int _getpid(void)                        { return 1; }
int _kill(int pid, int sig)              { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status)                   { (void)status; while (1) { } }
