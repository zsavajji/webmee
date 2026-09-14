/* Small runtime shims for glibc 2.10 gaps. Linked into WebKit via -ln9compat. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/prctl.h>
#include <errno.h>

/* pthread_setname_np arrived in glibc 2.12. prctl(PR_SET_NAME) does the same
 * job for the CALLING thread, and PR_SET_NAME exists in this sysroot.
 *
 * Naming a *different* thread needs its kernel tid, and glibc 2.10 exposes no
 * portable pthread_t -> tid mapping, so that case returns success without doing
 * anything. Thread naming is a debugging aid, never a correctness requirement,
 * so a partial implementation is the right trade here -- it keeps names for the
 * threads that name themselves, which is how WTF and bmalloc normally do it.
 */
int n9_pthread_setname_np(unsigned long thread, const char *name)
{
    if ((pthread_t) thread == pthread_self())
        return prctl(PR_SET_NAME, (unsigned long) name, 0, 0, 0) == 0 ? 0 : errno;
    return 0;
}
