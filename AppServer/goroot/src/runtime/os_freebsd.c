// Copyright 2011 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"
#include "defs_GOOS_GOARCH.h"
#include "os_GOOS.h"
#include "signal_unix.h"
#include "stack.h"
#include "textflag.h"

extern SigTab runtime·sigtab[];
extern int32 runtime·sys_umtx_op(uint32*, int32, uint32, void*, void*);

// From FreeBSD's <sys/sysctl.h>
#define	CTL_HW	6
#define	HW_NCPU	3

static Sigset sigset_none;
static Sigset sigset_all = { ~(uint32)0, ~(uint32)0, ~(uint32)0, ~(uint32)0, };

static int32
getncpu(void)
{
	uint32 mib[2];
	uint32 out;
	int32 ret;
	uintptr nout;

	// Fetch hw.ncpu via sysctl.
	mib[0] = CTL_HW;
	mib[1] = HW_NCPU;
	nout = sizeof out;
	out = 0;
	ret = runtime·sysctl(mib, 2, (byte*)&out, &nout, nil, 0);
	if(ret >= 0)
		return out;
	else
		return 1;
}

// FreeBSD's umtx_op syscall is effectively the same as Linux's futex, and
// thus the code is largely similar. See linux/thread.c and lock_futex.c for comments.

static void futexsleep(void);

#pragma textflag NOSPLIT
void
runtime·futexsleep(uint32 *addr, uint32 val, int64 ns)
{
	void (*fn)(void);

	g->m->ptrarg[0] = addr;
	g->m->scalararg[0] = val;
	g->m->ptrarg[1] = &ns;

	fn = futexsleep;
	runtime·onM(&fn);
}

static void
futexsleep(void)
{
	uint32 *addr;
	uint32 val;
	int64 ns;
	int32 ret;
	Timespec ts;
	
	addr = g->m->ptrarg[0];
	val = g->m->scalararg[0];
	ns = *(int64*)g->m->ptrarg[1];
	g->m->ptrarg[0] = nil;
	g->m->scalararg[0] = 0;
	g->m->ptrarg[1] = nil;

	if(ns < 0) {
		ret = runtime·sys_umtx_op(addr, UMTX_OP_WAIT_UINT_PRIVATE, val, nil, nil);
		if(ret >= 0 || ret == -EINTR)
			return;
		goto fail;
	}
	// NOTE: tv_nsec is int64 on amd64, so this assumes a little-endian system.
	ts.tv_nsec = 0;
	ts.tv_sec = runtime·timediv(ns, 1000000000, (int32*)&ts.tv_nsec);
	ret = runtime·sys_umtx_op(addr, UMTX_OP_WAIT_UINT_PRIVATE, val, nil, &ts);
	if(ret >= 0 || ret == -EINTR)
		return;

fail:
	runtime·prints("umtx_wait addr=");
	runtime·printpointer(addr);
	runtime·prints(" val=");
	runtime·printint(val);
	runtime·prints(" ret=");
	runtime·printint(ret);
	runtime·prints("\n");
	*(int32*)0x1005 = 0x1005;
}

static void badfutexwakeup(void);

#pragma textflag NOSPLIT
void
runtime·futexwakeup(uint32 *addr, uint32 cnt)
{
	int32 ret;
	void (*fn)(void);

	ret = runtime·sys_umtx_op(addr, UMTX_OP_WAKE_PRIVATE, cnt, nil, nil);
	if(ret >= 0)
		return;

	g->m->ptrarg[0] = addr;
	g->m->scalararg[0] = ret;
	fn = badfutexwakeup;
	if(g == g->m->gsignal)
		fn();
	else
		runtime·onM(&fn);
	*(int32*)0x1006 = 0x1006;
}

static void
badfutexwakeup(void)
{
	void *addr;
	int32 ret;
	
	addr = g->m->ptrarg[0];
	ret = g->m->scalararg[0];
	runtime·printf("umtx_wake addr=%p ret=%d\n", addr, ret);
}

void runtime·thr_start(void*);

void
runtime·newosproc(M *mp, void *stk)
{
	ThrParam param;
	Sigset oset;

	if(0){
		runtime·printf("newosproc stk=%p m=%p g=%p id=%d/%d ostk=%p\n",
			stk, mp, mp->g0, mp->id, (int32)mp->tls[0], &mp);
	}

	runtime·sigprocmask(&sigset_all, &oset);
	runtime·memclr((byte*)&param, sizeof param);

	param.start_func = runtime·thr_start;
	param.arg = (byte*)mp;
	
	// NOTE(rsc): This code is confused. stackbase is the top of the stack
	// and is equal to stk. However, it's working, so I'm not changing it.
	param.stack_base = (void*)mp->g0->stack.hi;
	param.stack_size = (byte*)stk - (byte*)mp->g0->stack.hi;

	param.child_tid = (void*)&mp->procid;
	param.parent_tid = nil;
	param.tls_base = (void*)&mp->tls[0];
	param.tls_size = sizeof mp->tls;

	mp->tls[0] = mp->id;	// so 386 asm can find it

	runtime·thr_new(&param, sizeof param);
	runtime·sigprocmask(&oset, nil);
}

void
runtime·osinit(void)
{
	runtime·ncpu = getncpu();
}

#pragma textflag NOSPLIT
void
runtime·get_random_data(byte **rnd, int32 *rnd_len)
{
	#pragma dataflag NOPTR
	static byte urandom_data[HashRandomBytes];
	int32 fd;
	fd = runtime·open("/dev/urandom", 0 /* O_RDONLY */, 0);
	if(runtime·read(fd, urandom_data, HashRandomBytes) == HashRandomBytes) {
		*rnd = urandom_data;
		*rnd_len = HashRandomBytes;
	} else {
		*rnd = nil;
		*rnd_len = 0;
	}
	runtime·close(fd);
}

void
runtime·goenvs(void)
{
	runtime·goenvs_unix();
}

// Called to initialize a new m (including the bootstrap m).
// Called on the parent thread (main thread in case of bootstrap), can allocate memory.
void
runtime·mpreinit(M *mp)
{
	mp->gsignal = runtime·malg(32*1024);
	mp->gsignal->m = mp;
}

// Called to initialize a new m (including the bootstrap m).
// Called on the new thread, can not allocate memory.
void
runtime·minit(void)
{
	// Initialize signal handling
	runtime·signalstack((byte*)g->m->gsignal->stack.lo, 32*1024);
	runtime·sigprocmask(&sigset_none, nil);
}

// Called from dropm to undo the effect of an minit.
void
runtime·unminit(void)
{
	runtime·signalstack(nil, 0);
}

uintptr
runtime·memlimit(void)
{
	Rlimit rl;
	extern byte runtime·text[], runtime·end[];
	uintptr used;
	
	if(runtime·getrlimit(RLIMIT_AS, &rl) != 0)
		return 0;
	if(rl.rlim_cur >= 0x7fffffff)
		return 0;

	// Estimate our VM footprint excluding the heap.
	// Not an exact science: use size of binary plus
	// some room for thread stacks.
	used = runtime·end - runtime·text + (64<<20);
	if(used >= rl.rlim_cur)
		return 0;

	// If there's not at least 16 MB left, we're probably
	// not going to be able to do much.  Treat as no limit.
	rl.rlim_cur -= used;
	if(rl.rlim_cur < (16<<20))
		return 0;

	return rl.rlim_cur - used;
}

extern void runtime·sigtramp(void);

typedef struct sigaction {
	union {
		void    (*__sa_handler)(int32);
		void    (*__sa_sigaction)(int32, Siginfo*, void *);
	} __sigaction_u;		/* signal handler */
	int32	sa_flags;		/* see signal options below */
	Sigset	sa_mask;		/* signal mask to apply */
} SigactionT;

void
runtime·setsig(int32 i, GoSighandler *fn, bool restart)
{
	SigactionT sa;

	runtime·memclr((byte*)&sa, sizeof sa);
	sa.sa_flags = SA_SIGINFO|SA_ONSTACK;
	if(restart)
		sa.sa_flags |= SA_RESTART;
	sa.sa_mask.__bits[0] = ~(uint32)0;
	sa.sa_mask.__bits[1] = ~(uint32)0;
	sa.sa_mask.__bits[2] = ~(uint32)0;
	sa.sa_mask.__bits[3] = ~(uint32)0;
	if(fn == runtime·sighandler)
		fn = (void*)runtime·sigtramp;
	sa.__sigaction_u.__sa_sigaction = (void*)fn;
	runtime·sigaction(i, &sa, nil);
}

GoSighandler*
runtime·getsig(int32 i)
{
	SigactionT sa;

	runtime·memclr((byte*)&sa, sizeof sa);
	runtime·sigaction(i, nil, &sa);
	if((void*)sa.__sigaction_u.__sa_sigaction == runtime·sigtramp)
		return runtime·sighandler;
	return (void*)sa.__sigaction_u.__sa_sigaction;
}

void
runtime·signalstack(byte *p, int32 n)
{
	StackT st;

	st.ss_sp = (void*)p;
	st.ss_size = n;
	st.ss_flags = 0;
	if(p == nil)
		st.ss_flags = SS_DISABLE;
	runtime·sigaltstack(&st, nil);
}

void
runtime·unblocksignals(void)
{
	runtime·sigprocmask(&sigset_none, nil);
}

#pragma textflag NOSPLIT
int8*
runtime·signame(int32 sig)
{
	return runtime·sigtab[sig].name;
}
