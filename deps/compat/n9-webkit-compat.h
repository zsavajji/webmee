/* Force-included into the WebKit build only (see deps/build-webkit.sh).
 * Declares the glibc 2.10 shims implemented in n9compat.c. Includes no system
 * header, for the reasons documented in n9-longdouble.h. */
#ifndef N9_WEBKIT_COMPAT_H
#define N9_WEBKIT_COMPAT_H
#ifndef __ASSEMBLER__

#ifdef __cplusplus
extern "C" {
#endif
int n9_pthread_setname_np(unsigned long thread, const char *name);
#ifdef __cplusplus
}
#endif

/* glibc 2.12+ only; route it to the prctl-based shim. */
#define pthread_setname_np(t, n) n9_pthread_setname_np((unsigned long)(t), (n))

#endif /* !__ASSEMBLER__ */
#endif
