#ifndef _SYS_KLOG_H
#define _SYS_KLOG_H 1
#ifdef __cplusplus
extern "C" {
#endif

#define KLOG_BUF_SZ 4096

int dmesg_read(char* buf, unsigned size);
int dmesg_clear(void);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_KLOG_H */
