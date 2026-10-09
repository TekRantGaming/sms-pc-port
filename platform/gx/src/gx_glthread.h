// The GL thread. While it runs it owns the GL context, and the GL entry
// points the rest of sms_gx calls (gl_funcs.h) are proxies that queue their
// work for it in order: calls without results return at once, calls that
// return something (or fill memory the caller reads) wait for the GL thread to
// get there. So the driver's time per draw and state change leaves the game's
// thread, which only waits where it needs an answer.
//
// The queue has one producer at a time (game threads run one at a time; see
// platform/os) and one consumer. SMS_GX_GL_THREAD=0 keeps GL on the game's
// thread.
#ifndef SMS_GX_GLTHREAD_H
#define SMS_GX_GLTHREAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <type_traits>
#include <utility>

namespace gx {
namespace glt {

bool active();
double waitedSeconds();  // how long the game's threads have waited for the GL thread
// True on the GL thread, and on any thread while there is none: then GL may
// be called directly.
bool onGlThread();

// Starts the GL thread, which first calls bind(arg) to make the context
// current there (the caller has released it). Returns false (and runs nothing)
// when the GL thread is disabled.
bool start(void (*bind)(void*), void* arg);
// Runs everything queued and ends the GL thread (at exit).
void stop();

typedef void (*Thunk)(void* payload);
enum : size_t { kMaxInline = 4u << 20 };  // larger data is not copied: the call waits instead
// A command of `bytes` payload (16-byte aligned) that the GL thread passes to fn.
void* reserve(Thunk fn, size_t bytes);
void publish();
// Waits until `done` is set (by a command the GL thread runs).
void waitFlag(const std::atomic<int>& done);
void signalFlag(std::atomic<int>& done);
void finish();  // waits until everything queued so far has run

template <class F> void callThunk(void* p) { (*static_cast<F*>(p))(); }

inline size_t align16(size_t n) { return (n + 15) & ~size_t(15); }

// Runs f on the GL thread, after everything queued before it.
template <class F> void post(const F& f) {
    static_assert(std::is_trivially_copyable<F>::value, "queued commands are copied bytewise");
    if (onGlThread()) {
        f();
        return;
    }
    void* p = reserve(&callThunk<F>, sizeof(F));
    memcpy(p, &f, sizeof(F));
    publish();
}

template <class F> void dataThunk(void* p) {
    F* f = static_cast<F*>(p);
    (*f)(static_cast<const uint8_t*>(p) + align16(sizeof(F)));
}
// Runs f(copy) on the GL thread, where copy holds the n bytes at `data` as
// they are now. Large data is not copied: f(data) runs and the call waits.
template <class F> void postData(const void* data, size_t n, const F& f);

// Runs f on the GL thread and waits for it (f may then use the caller's
// locals by reference).
template <class F> void sync(F&& f) {
    if (onGlThread()) {
        f();
        return;
    }
    typedef typename std::remove_reference<F>::type Fn;
    struct Call {
        Fn* f;
        std::atomic<int>* done;
        void operator()() const {
            (*f)();
            signalFlag(*done);
        }
    };
    std::atomic<int> done(0);
    post(Call{&f, &done});
    waitFlag(done);
}

template <class F> void postData(const void* data, size_t n, const F& f) {
    static_assert(std::is_trivially_copyable<F>::value, "queued commands are copied bytewise");
    if (onGlThread() || n > kMaxInline) {
        sync([&] { f(static_cast<const uint8_t*>(data)); });
        return;
    }
    uint8_t* p = static_cast<uint8_t*>(reserve(&dataThunk<F>, align16(sizeof(F)) + n));
    memcpy(p, &f, sizeof(F));
    if (n) memcpy(p + align16(sizeof(F)), data, n);
    publish();
}

// Installs the proxies over the loaded GL entry points (gl_loader.cpp).
void installProxies();

}  // namespace glt
}  // namespace gx

#endif
