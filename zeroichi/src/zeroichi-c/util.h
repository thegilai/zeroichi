/* Zero Ichi - small helpers: time, files, logging */
#ifndef ZEROICHI_UTIL_H
#define ZEROICHI_UTIL_H
#include <stddef.h>
#include <stdint.h>

double mono(void);                         /* seconds, monotonic */
double uptime(void);                       /* seconds since the kernel started */
double thread_cpu(void);
void msleep(int ms);
void sleep_s(double s);

char *read_file(const char *path, size_t *len);          /* malloc'd, 0-terminated */
int write_file(const char *path, const void *data, size_t len, int fsync_it);
int write_file_atomic(const char *path, const void *data, size_t len);
int file_exists(const char *path);
long long file_size(const char *path);
double file_mtime(const char *path);
int mkdir_p(const char *path);
int is_mount(const char *path);
long long free_bytes(const char *path);                  /* -1 = unknown */
long long total_bytes(const char *path);
int copy_file(const char *src, const char *dst);
void sync_all(void);
void sys_write(int on);                     /* 1: the system partition writable for a moment, 0: back */
int run_cmd(const char *cmd);                            /* system(), exit code */

/* The SD card's drives read-only whenever nothing is being written (the fast
 * start): sd_hold/sd_release around writes (write_file, write_file_atomic,
 * mkdir_p, sd_unlink and sd_rename do it themselves); its own thread makes
 * them read-only again. */
void sd_init(const char *dir1, const char *dir2);   /* the drives these folders are on */
int sd_active(void);
void sd_drives(char *out, size_t cap);
int sd_path(const char *path);                      /* on one of those drives? */
void sd_hold(void);
void sd_release(void);
void sd_started(void);                              /* (start-up writes done: read-only from now) */
int sd_writable(void);                              /* (being written now: don't unplug) */
void sd_end(void);                                  /* leaving: writable again */
int sd_unlink(const char *path);
int sd_rename(const char *a, const char *b);
int sd_rmdir(const char *path);

/* logging: zeroichi-log.txt (kept in memory until the SD card is ready) */
void log_open(const char *path, int append);   /* (append: carries on after a restart) */
void logi(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void loge(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_flush(void);

/* growable byte buffer */
typedef struct { uint8_t *p; size_t n, cap; } Buf;
void buf_add(Buf *b, const void *d, size_t n);
void buf_u8(Buf *b, int v);
void buf_be16(Buf *b, int v);
void buf_be32(Buf *b, uint32_t v);
void buf_le16(Buf *b, int v);
void buf_free(Buf *b);
#endif
