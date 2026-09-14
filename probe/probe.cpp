// n9 glibc-floor + ICU canary.
//
// One binary, run on device, answers the questions that decide whether a modern
// C++ engine can live on this hardware at all:
//
//   1. does a GCC-14 / static-libstdc++ binary exec at all on glibc 2.10?
//   2. does thread_local with a non-trivial destructor work?  (needs
//      __cxa_thread_atexit_impl, which glibc 2.10 does NOT export -- libstdc++
//      is supposed to fall back, this proves whether it actually does)
//   3. do std::thread / mutex / condvar work on Linux 2.6.32?
//   4. does exception unwinding cross a thread boundary? (ARM EHABI)
//   5. is there a usable cross-process shared-memory primitive for the
//      engine->shell framebuffer?  (shm_open, and a plain file fallback)
//   6. does statically-linked ICU load its data and do real Unicode work?
//
// Any FAIL here kills or reshapes the port, so each check prints its own line.

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <gnu/libc-version.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <unicode/uversion.h>
#include <unicode/unistr.h>
#include <unicode/normalizer2.h>
#include <unicode/coll.h>

static int failures = 0;
static void report(const char *name, bool ok, const std::string &detail = {})
{
    std::printf("[%s] %-28s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok) ++failures;
}

// ---- 2. thread_local with a non-trivial destructor --------------------------
static bool tls_dtor_ran = false;
struct TlsProbe {
    int v = 0;
    ~TlsProbe() { tls_dtor_ran = true; }
};
static thread_local TlsProbe g_tls;

// ---- 4. exception across a thread boundary ----------------------------------
static bool caught_in_thread = false;
static void throwing()
{
    try {
        throw std::string("unwind me");
    } catch (const std::string &s) {
        caught_in_thread = (s == "unwind me");
    }
}

int main()
{
    std::printf("runtime glibc : %s\n", gnu_get_libc_version());
    std::printf("ICU version   : %s\n", U_ICU_VERSION);
    std::printf("page size     : %ld\n\n", sysconf(_SC_PAGESIZE));

    // 3 + 2 + 4: threading, TLS destruction, unwinding
    {
        std::mutex m;
        std::condition_variable cv;
        bool started = false;
        std::thread t([&] {
            g_tls.v = 42;                       // force the TLS object to exist
            throwing();
            { std::lock_guard<std::mutex> lk(m); started = true; }
            cv.notify_one();
        });
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return started; });
        }
        t.join();
        report("std::thread + condvar", started);
        report("thread_local dtor", tls_dtor_ran,
               tls_dtor_ran ? "" : "__cxa_thread_atexit fallback missing");
        report("exception unwind (thread)", caught_in_thread);
    }

    // 5a: POSIX shared memory -- the intended engine->shell framebuffer transport
    {
        const char *name = "/n9probe";
        const size_t sz = 64 * 1024;
        bool ok = false;
        std::string detail;
        int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            detail = std::string("shm_open: ") + std::strerror(errno);
        } else if (ftruncate(fd, sz) != 0) {
            detail = std::string("ftruncate: ") + std::strerror(errno);
        } else {
            void *p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p == MAP_FAILED) {
                detail = std::string("mmap: ") + std::strerror(errno);
            } else {
                std::memset(p, 0xAB, sz);
                ok = (static_cast<unsigned char *>(p)[sz - 1] == 0xAB);
                munmap(p, sz);
            }
            close(fd);
            shm_unlink(name);
        }
        report("shm_open + mmap", ok, detail);
    }

    // 5b: file-backed MAP_SHARED across fork -- the fallback if /dev/shm is absent
    {
        char path[] = "/tmp/n9probeXXXXXX";
        const size_t sz = 4096;
        bool ok = false;
        std::string detail;
        int fd = mkstemp(path);
        if (fd < 0) {
            detail = std::string("mkstemp: ") + std::strerror(errno);
        } else if (ftruncate(fd, sz) != 0) {
            detail = std::string("ftruncate: ") + std::strerror(errno);
        } else {
            auto *p = static_cast<volatile unsigned char *>(
                mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            if (p == MAP_FAILED) {
                detail = std::string("mmap: ") + std::strerror(errno);
            } else {
                p[0] = 0;
                pid_t pid = fork();
                if (pid == 0) { p[0] = 0x5A; _exit(0); }       // child writes
                int st = 0;
                waitpid(pid, &st, 0);
                ok = (p[0] == 0x5A);                            // parent sees it
                munmap(const_cast<unsigned char *>(p), sz);
            }
            close(fd);
            unlink(path);
        }
        report("file mmap across fork", ok, detail);
    }

    // 6: ICU actually loading its statically-linked data
    {
        UErrorCode ec = U_ZERO_ERROR;
        const icu::Normalizer2 *nfc = icu::Normalizer2::getNFCInstance(ec);
        bool ok = U_SUCCESS(ec) && nfc;
        std::string detail = ok ? "" : u_errorName(ec);
        if (ok) {
            // "e" + combining acute  ->  single precomposed "e-acute"
            icu::UnicodeString in = icu::UnicodeString::fromUTF8("e\xCC\x81");
            icu::UnicodeString out = nfc->normalize(in, ec);
            ok = U_SUCCESS(ec) && out.length() == 1 && out.charAt(0) == 0x00E9;
            if (!ok) detail = "NFC composition wrong";
        }
        report("ICU data + NFC normalize", ok, detail);

        ec = U_ZERO_ERROR;
        icu::Collator *coll = icu::Collator::createInstance(icu::Locale("de_DE"), ec);
        // German collation must order a-umlaut between a and b
        bool cok = U_SUCCESS(ec) && coll
                   && coll->compare(icu::UnicodeString::fromUTF8("a"),
                                    icu::UnicodeString::fromUTF8("\xC3\xA4")) < 0
                   && coll->compare(icu::UnicodeString::fromUTF8("\xC3\xA4"),
                                    icu::UnicodeString::fromUTF8("b")) < 0;
        report("ICU locale collation", cok, U_SUCCESS(ec) ? "" : u_errorName(ec));
        delete coll;

        // Locale-sensitive casing -- WebKit leans on this for text-transform
        icu::UnicodeString tr = icu::UnicodeString::fromUTF8("i");
        tr.toUpper(icu::Locale("tr_TR"));      // Turkish: i -> dotted capital I
        report("ICU locale casing", tr.length() == 1 && tr.charAt(0) == 0x0130);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "NOT CLEAR" : "ALL CLEAR",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
