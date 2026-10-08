// OS threads, scheduler, interrupts, message queues, mutexes and condition
// variables on host threads.
//
// Model: one emulated CPU. Every OSThread runs on its own host thread, but only
// the thread that owns the CPU (g_cur) executes game code; it holds g_cpu the
// whole time and gives it up only inside a thread switch. Switches happen when
// the running thread blocks, yields, suspends/exits, or when an interrupt (VI
// retrace, deferred DVD/ARQ completions) wakes a higher-priority thread. This
// keeps the GameCube's uniprocessor, strict-priority semantics that the game's
// locking relies on.
#include "port_compat.h"
#include "port_win64_stack.h"
#include "crash.h"
#include "port_os.h"
#include "port_platform.h"
#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSMutex.h>
#include <pthread.h>
#include <atomic>
#include <deque>
#include <map>
#include <vector>
#include <time.h>

namespace {

struct Host {
	pthread_t th;
	pthread_cond_t cv;
	void* (*func)(void*);
	void* arg;
	bool started;
	bool cancelled;
	bool irq_enabled; // saved interrupt-enable state while switched out
	u64 last_ran;
};

pthread_mutex_t g_cpu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_idle_cv = PTHREAD_COND_INITIALIZER;
OSThread* g_cur;
OSThread g_default_thread;
std::map<OSThread*, Host*> g_hosts;
std::vector<OSThread*> g_threads;
u64 g_switch_seq;
bool g_irq_enabled = true;
bool g_in_irq;
bool g_need_resched;
int g_sched_disabled;
std::atomic<int> g_irq_kicks(0);
std::deque<std::function<void()> > g_deferred;
std::vector<port_irq_poll_fn> g_sources;

Host* host_of(OSThread* t)
{
	std::map<OSThread*, Host*>::iterator it = g_hosts.find(t);
	return it == g_hosts.end() ? NULL : it->second;
}

bool runnable(OSThread* t)
{
	return (t->state == OS_THREAD_STATE_READY || t->state == OS_THREAD_STATE_RUNNING) && t->suspend <= 0;
}

// Highest priority (lowest number) runnable thread; among equals the one that
// ran longest ago, except that `self` is kept unless `yield`.
OSThread* pick(OSThread* self, bool yield)
{
	OSThread* best = NULL;
	for (size_t i = 0; i < g_threads.size(); i++) {
		OSThread* t = g_threads[i];
		if (!runnable(t))
			continue;
		if (!best || t->priority < best->priority) {
			best = t;
			continue;
		}
		if (t->priority == best->priority) {
			bool tSelf = t == self, bSelf = best == self;
			if (bSelf && !yield)
				continue;
			if (tSelf && !yield) {
				best = t;
				continue;
			}
			if (bSelf && yield) {
				best = t;
				continue;
			}
			if (tSelf && yield)
				continue;
			if (host_of(t)->last_ran < host_of(best)->last_ran)
				best = t;
		}
	}
	return best;
}

void queue_push(OSThreadQueue* q, OSThread* t)
{
	// Priority order, FIFO among equals (as the SDK does).
	OSThread* at = q->head;
	while (at && at->priority <= t->priority)
		at = at->link.next;
	if (!at) {
		t->link.prev = q->tail;
		t->link.next = NULL;
		if (q->tail)
			q->tail->link.next = t;
		else
			q->head = t;
		q->tail = t;
	} else {
		t->link.next = at;
		t->link.prev = at->link.prev;
		if (at->link.prev)
			at->link.prev->link.next = t;
		else
			q->head = t;
		at->link.prev = t;
	}
	t->queue = q;
}

void queue_remove(OSThreadQueue* q, OSThread* t)
{
	if (t->link.next)
		t->link.next->link.prev = t->link.prev;
	else
		q->tail = t->link.prev;
	if (t->link.prev)
		t->link.prev->link.next = t->link.next;
	else
		q->head = t->link.next;
	t->link.next = t->link.prev = NULL;
	t->queue = NULL;
}

void* host_entry(void* p);

void deliver_irqs();

#if UINTPTR_MAX > 0xFFFFFFFFu
// 64-bit hosts: game code keeps pointers to its locals in u32 slots, so every
// host thread runs on a stack below 2 GiB (port_low_alloc). The threads are
// joinable so a stack is reused only once its last thread has fully exited.
enum { kLowStackSize = 1 << 20 };
struct LowStack {
	void* base;
	pthread_t th;
	bool used;
	void* argument;
};
std::vector<LowStack*> g_low_stacks;

LowStack* low_stack()
{
	for (LowStack* ls : g_low_stacks) {
		if (!ls->used)
			return ls->used = true, ls;
#ifdef __GLIBC__
		if (pthread_tryjoin_np(ls->th, NULL) == 0)
			return ls;
#elif defined(_WIN64)
		if (_pthread_tryjoin(ls->th, NULL) == 0)
			return ls;
#endif
	}
	void* p = port_low_alloc(kLowStackSize);
	if (!p) {
		port_log("[os] no memory below 2 GiB for a thread stack\n");
		abort();
	}
	LowStack* ls = new LowStack{p, pthread_t(), true, NULL};
	g_low_stacks.push_back(ls);
	return ls;
}
#ifdef _WIN64
void* windows_host_entry(void* argument)
{
	LowStack* stack = (LowStack*)argument;
	return port_win64_stack_call(stack->base, kLowStackSize, host_entry, stack->argument);
}
#endif
#endif

// Hand the CPU to `next` (already chosen). Returns once `self` owns the CPU
// again, or never if `self` is exiting.
void switch_to(OSThread* self, OSThread* next, bool exiting)
{
	Host* hs = self ? host_of(self) : NULL;
	if (hs)
		hs->irq_enabled = g_irq_enabled;
	g_cur = next;
	__gCurrentThread = next;
	Host* hn = host_of(next);
	next->state = OS_THREAD_STATE_RUNNING;
	hn->last_ran = ++g_switch_seq;
	g_irq_enabled = hn->irq_enabled;
	if (!hn->started) {
		hn->started = true;
		pthread_attr_t a;
		pthread_attr_init(&a);
#if UINTPTR_MAX > 0xFFFFFFFFu
		LowStack* ls = low_stack();
#ifdef _WIN64
		pthread_attr_setstacksize(&a, kLowStackSize);
		ls->argument = next;
#else
		pthread_attr_setstack(&a, ls->base, kLowStackSize);
#endif
#else
		pthread_attr_setstacksize(&a, 1 << 20);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
#endif
#ifdef _WIN64
		int create_result = pthread_create(&hn->th, &a, windows_host_entry, ls);
#else
		int create_result = pthread_create(&hn->th, &a, host_entry, next);
#endif
		if (create_result != 0) {
			port_log("[os] pthread_create failed\n");
			abort();
		}
#if UINTPTR_MAX > 0xFFFFFFFFu
		ls->th = hn->th;
#endif
		pthread_attr_destroy(&a);
	} else {
		pthread_cond_signal(&hn->cv);
	}
	if (exiting) {
		pthread_mutex_unlock(&g_cpu);
#ifdef _WIN64
		port_win64_thread_exit();
#else
		pthread_exit(NULL);
#endif
	}
	while (g_cur != self) {
		pthread_cond_wait(&hs->cv, &g_cpu);
		// A cancelled host never runs again, even when its OSThread object
		// has since been recreated and scheduled (it then has a new Host).
		if (hs->cancelled && (g_cur != self || host_of(self) != hs)) {
			pthread_mutex_unlock(&g_cpu);
#ifdef _WIN64
			port_win64_thread_exit();
#else
			pthread_exit(NULL);
#endif
		}
	}
	g_irq_enabled = hs->irq_enabled;
}

// Wall time with no runnable game thread (the overlay's frame breakdown).
double g_idle_seconds;
double mono_seconds()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Give up the CPU if someone better is runnable (or, with `yield`, an equal).
// Blocks in the idle loop while nothing is runnable.
void reschedule(bool yield)
{
	OSThread* self = g_cur;
	OSThread* next;
	double idle_from = 0;
	for (;;) {
		deliver_irqs();
		next = pick(self, yield);
		if (next)
			break;
		// Idle: every thread is blocked. Wait for an interrupt source (or, with
		// a deterministic clock, make the next retrace happen now).
		if (port_vi_idle_advance())
			continue;
		if (idle_from == 0)
			idle_from = mono_seconds();
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 1000000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&g_idle_cv, &g_cpu, &ts);
	}
	if (idle_from != 0)
		g_idle_seconds += mono_seconds() - idle_from;
	g_need_resched = false;
	if (next == self) {
		self->state = OS_THREAD_STATE_RUNNING;
		return;
	}
	if (self->state == OS_THREAD_STATE_RUNNING)
		self->state = OS_THREAD_STATE_READY;
	switch_to(self, next, false);
}

void preempt_check()
{
	if (g_in_irq || g_sched_disabled) {
		g_need_resched = true;
		return;
	}
	OSThread* next = pick(g_cur, false);
	if (next && next != g_cur)
		reschedule(false);
}

void deliver_irqs()
{
	if (g_in_irq)
		return;
	g_in_irq = true;
	g_irq_kicks.store(0);
	for (size_t i = 0; i < g_sources.size(); i++)
		g_sources[i]();
	while (!g_deferred.empty()) {
		std::function<void()> fn = g_deferred.front();
		g_deferred.pop_front();
		fn();
	}
	g_in_irq = false;
}

void* host_entry(void* p)
{
	port_crash_thread_init();
	OSThread* self = (OSThread*)p;
	pthread_mutex_lock(&g_cpu);
	Host* h = host_of(self);
	while (g_cur != self || host_of(self) != h) {
		pthread_cond_wait(&h->cv, &g_cpu);
		if (h->cancelled) { // cancelled before it ever ran
			pthread_mutex_unlock(&g_cpu);
			return NULL;
		}
	}
	g_irq_enabled = true;
	void* ret = h->func(h->arg);
	OSExitThread((OSThread*)ret);
	return NULL;
}

Host* new_host()
{
	Host* h = new Host();
	pthread_cond_init(&h->cv, NULL);
	h->irq_enabled = true;
	return h;
}

} // namespace

// ---------------------------------------------------------------------------
// Interrupt plumbing used by the other platform modules.

extern "C" void port_irq_kick(void)
{
	g_irq_kicks.fetch_add(1);
	pthread_cond_signal(&g_idle_cv);
}

extern "C" void port_irq_add_source(port_irq_poll_fn fn) { g_sources.push_back(fn); }

void port_irq_defer(std::function<void()> fn) { g_deferred.push_back(fn); }

// Run `fn` now, in interrupt context (used for "DMA" that completes
// instantly, where game code busy-waits on the completion callback's effect).
void port_irq_run_now(std::function<void()> fn)
{
	bool was = g_in_irq;
	g_in_irq = true;
	fn();
	g_in_irq = was;
	if (!was && g_irq_enabled)
		preempt_check();
}

extern "C" void port_irq_check(void)
{
	if (!g_irq_enabled || g_in_irq)
		return;
	if (g_irq_kicks.load() == 0 && g_deferred.empty())
		return;
	deliver_irqs();
	if (g_need_resched || true)
		preempt_check();
}

extern "C" void port_os_threads_init(void)
{
	pthread_mutex_lock(&g_cpu); // the boot thread owns the CPU from here on
	OSThread* t = &g_default_thread;
	memset(t, 0, sizeof(*t));
	t->state    = OS_THREAD_STATE_RUNNING;
	t->priority = t->base = 16;
	Host* h     = new_host();
	h->started  = true;
	h->th       = pthread_self();
	g_hosts[t]  = h;
	g_threads.push_back(t);
	g_cur            = t;
	__gCurrentThread = t;
}

// ---------------------------------------------------------------------------
// Interrupts

extern "C" BOOL OSDisableInterrupts(void)
{
	BOOL old      = g_irq_enabled;
	g_irq_enabled = false;
	return old;
}

extern "C" BOOL OSEnableInterrupts(void)
{
	BOOL old      = g_irq_enabled;
	g_irq_enabled = true;
	port_irq_check();
	return old;
}

extern "C" BOOL OSRestoreInterrupts(BOOL level)
{
	BOOL old      = g_irq_enabled;
	g_irq_enabled = level != 0;
	if (level)
		port_irq_check();
	return old;
}

// ---------------------------------------------------------------------------
// Threads

extern "C" void OSInitThreadQueue(OSThreadQueue* queue) { queue->head = queue->tail = NULL; }

extern "C" OSThread* OSGetCurrentThread(void) { return g_cur; }

extern "C" BOOL OSIsThreadTerminated(OSThread* t)
{
	return t->state == OS_THREAD_STATE_MORIBUND || t->state == 0;
}

extern "C" s32 OSDisableScheduler(void) { return g_sched_disabled++; }
extern "C" s32 OSEnableScheduler(void)
{
	s32 old = g_sched_disabled;
	if (g_sched_disabled > 0 && --g_sched_disabled == 0 && g_need_resched)
		preempt_check();
	return old;
}

extern "C" void OSYieldThread(void) { reschedule(true); }

extern "C" int OSCreateThread(OSThread* t, void* (*func)(void*), void* param, void* stack, u32 stackSize,
                              OSPriority prio, u16 attr)
{
	if (prio < OS_PRIORITY_MIN || prio > OS_PRIORITY_MAX)
		return FALSE;
	bool wasDead = t->state == OS_THREAD_STATE_MORIBUND;
	memset(&t->state, 0, sizeof(*t) - offsetof(OSThread, state));
	t->state     = OS_THREAD_STATE_READY;
	t->attr      = attr & OS_THREAD_ATTR_DETACH;
	t->suspend   = 1;
	t->priority  = t->base = prio;
	t->stackBase = (u8*)stack;
	t->stackEnd  = (u32*)((u8*)stack - stackSize);
	Host* h      = host_of(t);
	if (h && h->started && !wasDead) {
		port_log("[os] OSCreateThread reusing a live thread object %p\n", t);
	}
	if (h && h->started) {
		// The previous incarnation's host thread may still be parked on its
		// condition variable (OSCancelThread only asks it to leave; it goes
		// when it next wakes). Reusing its Host would clear `cancelled` and
		// let it resume the old function alongside the new one: the THP
		// decoder threads recreated for the next movie crashed that way.
		// Retire it and give the new incarnation a Host of its own.
		h->cancelled = true;
		pthread_cond_signal(&h->cv);
		h          = new_host();
		g_hosts[t] = h;
	}
	if (!h) {
		h          = new_host();
		g_hosts[t] = h;
		g_threads.push_back(t);
	}
	h->func      = func;
	h->arg       = param;
	h->started   = false;
	h->cancelled = false;
	return TRUE;
}

// The decomp header spells the parameter OSThread*; it is the exit value.
extern "C" void OSExitThread(OSThread* valArg)
{
	void* val      = valArg;
	OSThread* self = g_cur;
	self->val      = val;
	self->state    = OS_THREAD_STATE_MORIBUND;
	OSWakeupThread(&self->queueJoin);
	OSThread* next;
	for (;;) {
		deliver_irqs();
		next = pick(NULL, true);
		if (next)
			break;
		if (port_vi_idle_advance())
			continue;
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec = (ts.tv_nsec + 1000000) % 1000000000;
		pthread_cond_timedwait(&g_idle_cv, &g_cpu, &ts);
	}
	switch_to(self, next, true);
}

extern "C" void OSCancelThread(OSThread* t)
{
	if (t == g_cur) {
		OSExitThread(NULL);
		return;
	}
	if (t->state == OS_THREAD_STATE_WAITING && t->queue)
		queue_remove(t->queue, t);
	t->state = OS_THREAD_STATE_MORIBUND;
	Host* h  = host_of(t);
	if (h) {
		h->cancelled = true;
		if (h->started)
			pthread_cond_signal(&h->cv);
	}
	OSWakeupThread(&t->queueJoin);
}

extern "C" int OSJoinThread(OSThread* t, void** val)
{
	while (t->state != OS_THREAD_STATE_MORIBUND && t->state != 0)
		OSSleepThread(&t->queueJoin);
	if (val)
		*val = t->val;
	return TRUE;
}

extern "C" void OSDetachThread(OSThread* t) { t->attr |= OS_THREAD_ATTR_DETACH; }

extern "C" long OSResumeThread(OSThread* t)
{
	long old = t->suspend;
	if (--t->suspend < 0)
		t->suspend = 0;
	if (t->suspend == 0)
		preempt_check();
	return old;
}

extern "C" s32 OSSuspendThread(OSThread* t)
{
	s32 old = t->suspend++;
	if (t == g_cur && !g_in_irq)
		reschedule(false);
	return old;
}

extern "C" void OSSleepThread(OSThreadQueue* queue)
{
	OSThread* self = g_cur;
	if (g_in_irq) {
		port_log("[os] OSSleepThread called from interrupt context; ignored\n");
		return;
	}
	self->state = OS_THREAD_STATE_WAITING;
	queue_push(queue, self);
	reschedule(false);
}

extern "C" void OSWakeupThread(OSThreadQueue* queue)
{
	bool any = false;
	while (OSThread* t = queue->head) {
		queue_remove(queue, t);
		if (t->state == OS_THREAD_STATE_WAITING)
			t->state = OS_THREAD_STATE_READY;
		any = true;
	}
	if (any)
		preempt_check();
}

extern "C" long OSGetThreadPriority(OSThread* t) { return t->priority; }

extern "C" int OSSetThreadPriority(OSThread* t, OSPriority prio)
{
	if (prio < OS_PRIORITY_MIN || prio > OS_PRIORITY_MAX)
		return FALSE;
	t->priority = t->base = prio;
	preempt_check();
	return TRUE;
}

// ---------------------------------------------------------------------------
// Message queues

#define OS_MESSAGE_BLOCK 1

extern "C" void OSInitMessageQueue(OSMessageQueue* mq, void* msgArray, long msgCount)
{
	OSInitThreadQueue(&mq->queueSend);
	OSInitThreadQueue(&mq->queueReceive);
	mq->msgArray   = msgArray;
	mq->msgCount   = msgCount;
	mq->firstIndex = 0;
	mq->usedCount  = 0;
}

extern "C" int OSSendMessage(OSMessageQueue* mq, void* msg, long flags)
{
	BOOL lvl = OSDisableInterrupts();
	while (mq->msgCount <= mq->usedCount) {
		if (!(flags & OS_MESSAGE_BLOCK) || g_in_irq) {
			OSRestoreInterrupts(lvl);
			return FALSE;
		}
		OSSleepThread(&mq->queueSend);
	}
	long idx                     = (mq->firstIndex + mq->usedCount) % mq->msgCount;
	((void**)mq->msgArray)[idx] = msg;
	mq->usedCount++;
	OSWakeupThread(&mq->queueReceive);
	OSRestoreInterrupts(lvl);
	return TRUE;
}

extern "C" int OSReceiveMessage(OSMessageQueue* mq, void* msg, long flags)
{
	BOOL lvl = OSDisableInterrupts();
	while (mq->usedCount == 0) {
		if (!(flags & OS_MESSAGE_BLOCK) || g_in_irq) {
			OSRestoreInterrupts(lvl);
			return FALSE;
		}
		OSSleepThread(&mq->queueReceive);
	}
	if (msg)
		*(void**)msg = ((void**)mq->msgArray)[mq->firstIndex];
	mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
	mq->usedCount--;
	OSWakeupThread(&mq->queueSend);
	OSRestoreInterrupts(lvl);
	return TRUE;
}

extern "C" int OSJamMessage(OSMessageQueue* mq, void* msg, long flags)
{
	BOOL lvl = OSDisableInterrupts();
	while (mq->msgCount <= mq->usedCount) {
		if (!(flags & OS_MESSAGE_BLOCK) || g_in_irq) {
			OSRestoreInterrupts(lvl);
			return FALSE;
		}
		OSSleepThread(&mq->queueSend);
	}
	mq->firstIndex                            = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
	((void**)mq->msgArray)[mq->firstIndex] = msg;
	mq->usedCount++;
	OSWakeupThread(&mq->queueReceive);
	OSRestoreInterrupts(lvl);
	return TRUE;
}

// ---------------------------------------------------------------------------
// Mutexes and condition variables

extern "C" void OSInitMutex(OSMutex* m)
{
	OSInitThreadQueue(&m->queue);
	m->thread = NULL;
	m->count  = 0;
	m->link.next = m->link.prev = NULL;
}

extern "C" void OSLockMutex(OSMutex* m)
{
	OSThread* self = g_cur;
	for (;;) {
		if (!m->thread) {
			m->thread = self;
			m->count  = 1;
			return;
		}
		if (m->thread == self) {
			m->count++;
			return;
		}
		self->mutex = m;
		OSSleepThread(&m->queue);
		self->mutex = NULL;
	}
}

extern "C" void OSUnlockMutex(OSMutex* m)
{
	if (m->thread != g_cur)
		return;
	if (--m->count == 0) {
		m->thread = NULL;
		OSWakeupThread(&m->queue);
	}
}

extern "C" BOOL OSTryLockMutex(OSMutex* m)
{
	if (!m->thread) {
		m->thread = g_cur;
		m->count  = 1;
		return TRUE;
	}
	if (m->thread == g_cur) {
		m->count++;
		return TRUE;
	}
	return FALSE;
}

extern "C" void OSInitCond(OSCond* c) { OSInitThreadQueue(&c->queue); }

extern "C" void OSWaitCond(OSCond* c, OSMutex* m)
{
	if (m->thread != g_cur)
		return;
	s32 count = m->count;
	m->count  = 0;
	m->thread = NULL;
	OSWakeupThread(&m->queue);
	OSSleepThread(&c->queue);
	OSLockMutex(m);
	m->count = count;
}

extern "C" void OSSignalCond(OSCond* c) { OSWakeupThread(&c->queue); }

extern "C" double port_idle_seconds(void) { return g_idle_seconds; }
