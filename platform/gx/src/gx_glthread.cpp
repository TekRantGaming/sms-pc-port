// The GL thread and its command queue (see gx_glthread.h).
#include "gx_glthread.h"
#include "gx_internal.h"

#include <stdlib.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#endif

namespace gx {
namespace glt {

namespace {

// The queue is a ring of commands: a 16-byte header, then the payload. The
// producer and the consumer count bytes from the start (positions are the
// counts modulo the ring's size); a header with no function skips to the
// ring's start.
struct Header {
    Thunk fn;
    uint32_t size;  // header and payload, a multiple of 16
};
static_assert(sizeof(Header) <= 16, "command headers take 16 bytes");

const size_t kRing = size_t(16) << 20;  // a frame queues ~150 KiB; uploads up to kMaxInline
uint8_t* s_ring = nullptr;
uint64_t s_head = 0;                  // producer: bytes written
uint64_t s_reserved = 0;              // producer: end of the command being written
std::atomic<uint64_t> s_published{0};  // the commands before this are complete
std::atomic<uint64_t> s_tail{0};       // consumer: bytes done (the space before is free)
std::atomic<bool> s_running{false};
std::atomic<bool> s_quit{false};
thread_local bool t_onGl = false;
std::thread* s_thread = nullptr;

// Sleeping: the GL thread sleeps as soon as it runs out of commands, and the
// game's thread wakes it once commands worth kWakeBytes have collected, or at
// once when it needs an answer, so a frame wakes it a few times rather than
// keeping it spinning. The game's thread spins for a moment before it blocks.
const uint64_t kWakeBytes = 32u << 10;
uint64_t s_wokenAt = 0;  // producer: s_head when the GL thread was last woken
std::mutex s_mtx;
std::condition_variable s_glCv, s_mainCv;
std::atomic<bool> s_glSleeping{false}, s_mainSleeping{false};

inline void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

void wakeGl() {
    s_wokenAt = s_head;
    if (s_glSleeping.load()) {
        std::lock_guard<std::mutex> l(s_mtx);
        s_glCv.notify_one();
    }
}

void wakeMain() {
    if (s_mainSleeping.load()) {
        std::lock_guard<std::mutex> l(s_mtx);
        s_mainCv.notify_all();
    }
}

// Waits until cond() holds. On macOS's main thread the wait also runs the
// main run loop: SDL makes the GL thread's swap call into the main thread
// (dispatch_sync) after the window changed size.
double s_waited = 0;  // seconds the game's threads waited for the GL thread

template <class C> void mainWait(const C& cond) {
    if (cond()) return;
    const double t0 = monoSeconds();
    struct Timed {
        double t0;
        ~Timed() { s_waited += monoSeconds() - t0; }
    } timed{t0};
    wakeGl();  // what is queued must run
    for (int i = 0; i < 4000; i++) {  // ~0.1 ms
        if (cond()) return;
        relax();
    }
#ifdef __APPLE__
    if (pthread_main_np()) {
        while (!cond()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0002, true);
        return;
    }
#endif
    std::unique_lock<std::mutex> l(s_mtx);
    s_mainSleeping.store(true);
    while (!cond()) s_mainCv.wait_for(l, std::chrono::milliseconds(1));
    s_mainSleeping.store(false);
}

void threadMain(void (*bind)(void*), void* arg, std::atomic<int>* ready) {
    t_onGl = true;
    bind(arg);
    signalFlag(*ready);
    uint64_t tail = s_tail.load();
    for (;;) {
        uint64_t pub = s_published.load(std::memory_order_acquire);
        if (tail == pub) {
            if (s_quit.load()) break;
            bool more = false;
            for (int i = 0; i < 64 && !more; i++) {
                relax();
                more = s_published.load(std::memory_order_acquire) != tail;
            }
            if (!more) {
                std::unique_lock<std::mutex> l(s_mtx);
                s_glSleeping.store(true);
                while (s_published.load(std::memory_order_acquire) == tail && !s_quit.load())
                    s_glCv.wait_for(l, std::chrono::milliseconds(2));
                s_glSleeping.store(false);
            }
            continue;
        }
        while (tail != pub) {
            Header* h = reinterpret_cast<Header*>(s_ring + tail % kRing);
            if (h->fn) h->fn(reinterpret_cast<uint8_t*>(h) + 16);
            tail += h->size;
            s_tail.store(tail, std::memory_order_release);
        }
        wakeMain();  // a producer may wait for space
    }
}

bool enabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SMS_GX_GL_THREAD");
        on = !(e && e[0] == '0');
#if defined(_WIN32) && !defined(_WIN64)
        on = 0;  // the proxies do not follow 32-bit Windows' __stdcall GL entry points
#endif
    }
    return on != 0;
}

void stopAtExit() { stop(); }

}  // namespace

bool active() { return s_running.load(std::memory_order_relaxed); }
double waitedSeconds() { return s_waited; }
bool onGlThread() { return t_onGl || !s_running.load(std::memory_order_relaxed); }

void* reserve(Thunk fn, size_t bytes) {
    const size_t total = 16 + align16(bytes);
    if (total > kRing / 2) fatal("GL thread: a %zu-byte command does not fit its queue", bytes);
    size_t at = size_t(s_head % kRing);
    const size_t pad = at + total > kRing ? kRing - at : 0;  // the command does not fit before the end: wrap
    mainWait([&] { return s_head + pad + total - s_tail.load(std::memory_order_acquire) <= kRing; });
    if (pad) {
        Header* w = reinterpret_cast<Header*>(s_ring + at);
        w->fn = nullptr;
        w->size = uint32_t(pad);
        s_head += pad;
        at = 0;
    }
    Header* h = reinterpret_cast<Header*>(s_ring + at);
    h->fn = fn;
    h->size = uint32_t(total);
    s_reserved = s_head + total;
    return s_ring + at + 16;
}

void publish() {
    s_head = s_reserved;
    s_published.store(s_head, std::memory_order_release);
    if (s_head - s_wokenAt >= kWakeBytes) wakeGl();
}

void waitFlag(const std::atomic<int>& done) {
    mainWait([&] { return done.load(std::memory_order_acquire) != 0; });
}

void signalFlag(std::atomic<int>& done) {
    done.store(1, std::memory_order_release);
    wakeMain();
}

void finish() {
    if (!s_running.load() || t_onGl) return;
    const uint64_t upto = s_head;
    mainWait([&] { return s_tail.load(std::memory_order_acquire) >= upto; });
}

bool start(void (*bind)(void*), void* arg) {
    if (s_running.load() || !enabled()) return false;
    if (!s_ring) {
        s_ring = static_cast<uint8_t*>(malloc(kRing));
        if (!s_ring) return false;
    }
    s_quit.store(false);
    std::atomic<int> ready(0);
    s_thread = new std::thread(threadMain, bind, arg, &ready);
    // until the GL thread runs, this thread still counts as the GL thread (the
    // wait runs macOS's main run loop: SDL may make the GL thread's
    // MakeCurrent call into it)
    waitFlag(ready);
    s_running.store(true);
    static bool atExit = false;
    if (!atExit) {
        atExit = true;
        atexit(stopAtExit);
    }
    logmsg("GL thread running");
    return true;
}

void stop() {
    // exit() can come on the GL thread too (a driver giving up): it cannot
    // wait for itself, and the process ends anyway
    if (!s_running.load() || t_onGl) return;
    finish();
    s_quit.store(true);
    wakeGl();
    s_thread->join();
    delete s_thread;
    s_thread = nullptr;
    s_running.store(false);
}

}  // namespace glt
}  // namespace gx
