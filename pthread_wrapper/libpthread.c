#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <memory.h>
#include <pthread.h>
#include <semaphore.h>
#include <assert.h>
#include <sys/mman.h>
#include <setjmp.h>

// when __GLIBC__ (or some glibc specific symbol) is not defined, we're assuming musl; can't check to be sure because 🤡

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {{PTHREAD_MUTEX_RECURSIVE}}
#endif

#ifndef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP {{PTHREAD_MUTEX_ERRORCHECK}}
#endif

typedef struct {
	union {
		struct {
			unsigned int count;
#ifdef __LP64__
			int __reserved[3];
#endif
		} bionic;
		sem_t *glibc;
	};
} bionic_sem_t;

typedef struct {
	union {
		struct {
			uint32_t flags;
			void* stack_base;
			size_t stack_size;
			size_t guard_size;
			int32_t sched_policy;
			int32_t sched_priority;
#ifdef __LP64__
			char __reserved[16];
#endif
		} bionic;
		pthread_attr_t *glibc;
	};
} bionic_attr_t;

typedef struct {
	union {
#if defined(__LP64__)
		int32_t __private[10];
#else
		int32_t __private[1];
#endif
		pthread_mutex_t *glibc;
	};
} bionic_mutex_t;

typedef struct {
	union {
		long __private;
		pthread_mutexattr_t *glibc;
	};
} bionic_mutexattr_t;

typedef struct {
	union {
		#if defined(__LP64__)
		  int32_t __private[14];
		#else
		  int32_t __private[10];
		#endif
		pthread_rwlock_t *glibc;
	};
} bionic_rwlock_t;

static const struct {
	bionic_mutex_t bionic;
	pthread_mutex_t glibc;
} bionic_mutex_init_map[] = {
	{ .bionic = {{{ ((PTHREAD_MUTEX_NORMAL & 3) << 14) }}}, .glibc = PTHREAD_MUTEX_INITIALIZER },
	{ .bionic = {{{ ((PTHREAD_MUTEX_RECURSIVE & 3) << 14) }}}, .glibc = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP },
	{ .bionic = {{{ ((PTHREAD_MUTEX_ERRORCHECK & 3) << 14) }}}, .glibc = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP },
};

typedef struct {
	union {
#if defined(__LP64__)
		int32_t __private[12];
#else
		int32_t __private[1];
#endif
		pthread_cond_t *glibc;
	};
} bionic_cond_t;

typedef struct {
	union {
		long __private;
		pthread_condattr_t *glibc;
	};
} bionic_condattr_t;

typedef int bionic_key_t;
_Static_assert(sizeof(bionic_key_t) == sizeof(pthread_key_t), "bionic_key_t and pthread_key_t size mismatch");

typedef int bionic_once_t;
_Static_assert(sizeof(bionic_once_t) == sizeof(pthread_once_t), "bionic_once_t and pthread_once_t size mismatch");

typedef long bionic_pthread_t; // seems to be 32bit on 32bit musl, 64bit everywhere else, which is why long happens to work
_Static_assert(sizeof(bionic_pthread_t) == sizeof(pthread_t), "bionic_pthread_t and pthread_t size mismatch");

typedef uint64_t bionic_rwlockattr_t;
_Static_assert(sizeof(bionic_rwlockattr_t) == sizeof(pthread_rwlockattr_t), "bionic_rwlockattr_t and pthread_rwlockattr_t size mismatch");

struct bionic_pthread_cleanup_t {
	union {
		struct bionic_pthread_cleanup_t *prev;
#ifdef __GLIBC__
		__pthread_unwind_buf_t *glibc;
#else
		struct __ptcb *musl;
#endif
	};
	void (*routine)(void*);
	void *arg;
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

// For checking, if our glibc version is mapped to memory.
// Used for sanity checking and static initialization below.
#define IS_MAPPED(x) is_mapped(x->glibc, sizeof(x))

// For handling static initialization.
#define INIT_IF_NOT_MAPPED(x, init) do { if (!IS_MAPPED(x)) init(x); } while(0)

static bool is_mapped(void *mem, const size_t sz)
{
	const size_t ps = sysconf(_SC_PAGESIZE);
	assert(ps > 0);
	unsigned char vec[(sz + ps - 1) / ps];
	return !mincore(mem, sz, vec);
}

void bionic___pthread_cleanup_push(struct bionic_pthread_cleanup_t *c, void (*routine)(void*), void *arg)
{
	assert(c && routine);
#ifdef __GLIBC__
	c->glibc = mmap(NULL, sizeof(*c->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	c->routine = routine;
	c->arg = arg;

	int not_first_call;
	if ((not_first_call = sigsetjmp((struct __jmp_buf_tag*)(void*)c->glibc->__cancel_jmp_buf, 0))) {
		routine(arg);
		__pthread_unwind_next(c->glibc);
	}

	__pthread_register_cancel(c->glibc);
#else
	c->musl = mmap(NULL, sizeof(struct __ptcb), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	c->routine = routine;
	c->arg = arg;
	_pthread_cleanup_push(c->musl, routine, arg);
#endif
}

void bionic___pthread_cleanup_pop(struct bionic_pthread_cleanup_t *c, int execute)
{
#ifdef __GLIBC__
	assert(c && IS_MAPPED(c)); // TODO - analogically for musl?
	__pthread_unregister_cancel(c->glibc);

	if (execute)
		c->routine(c->arg);

	munmap(c->glibc, sizeof(*c->glibc));
#else
	_pthread_cleanup_pop(c->musl, execute);
	munmap(c->musl, sizeof(struct __ptcb));
#endif
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* sem */

int bionic_sem_destroy(bionic_sem_t *sem)
{
	assert(sem);
	int ret = 0;
	if (IS_MAPPED(sem)) {
		ret = sem_destroy(sem->glibc);
		munmap(sem->glibc, sizeof(*sem->glibc));
	}
	return ret;
}

static void default_sem_init(bionic_sem_t *sem)
{
	// Apparently some android apps (hearthstone) do not call sem_init()
	assert(sem);
	sem->glibc = mmap(NULL, sizeof(*sem->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(sem->glibc, 0, sizeof(*sem->glibc));
}

int bionic_sem_init(bionic_sem_t *sem, int pshared, unsigned int value)
{
	assert(sem);
	// From SEM_INIT(3)
	// Initializing a semaphore that has already been initialized results in underined behavior.
	*sem = (bionic_sem_t){0};
	sem->glibc = mmap(NULL, sizeof(*sem->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return sem_init(sem->glibc, pshared, value);
}

int bionic_sem_post(bionic_sem_t *sem)
{
	assert(sem);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_post(sem->glibc);
}

int bionic_sem_wait(bionic_sem_t *sem)
{
	assert(sem);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_wait(sem->glibc);
}

int bionic_sem_trywait(bionic_sem_t *sem)
{
	assert(sem);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_trywait(sem->glibc);
}

int bionic_sem_timedwait(bionic_sem_t *sem, const struct timespec *abs_timeout)
{
	assert(sem && abs_timeout);
	INIT_IF_NOT_MAPPED(sem, default_sem_init);
	return sem_timedwait(sem->glibc, abs_timeout);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* rwlock */

int bionic_pthread_rwlock_destroy(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	int ret = 0;
	if (IS_MAPPED(rwlock)) {
		ret = pthread_rwlock_destroy(rwlock->glibc);
		munmap(rwlock->glibc, sizeof(*rwlock->glibc));
	}
	return ret;

}

static void default_rwlock_init(bionic_rwlock_t *rwlock)
{
	// Apparently some android apps/libs (Qt5) do not call pthread_rwlock_init()
	assert(rwlock);
	rwlock->glibc = mmap(NULL, sizeof(*rwlock->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(rwlock->glibc, 0, sizeof(*rwlock->glibc));
}

int bionic_pthread_rwlock_init(bionic_rwlock_t *restrict rwlock, const bionic_rwlockattr_t *restrict attr)
{
	assert(rwlock);
	rwlock->glibc = mmap(NULL, sizeof(*rwlock->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_rwlock_init(rwlock->glibc, (pthread_rwlockattr_t *)attr);
}

int bionic_pthread_rwlock_rdlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	INIT_IF_NOT_MAPPED(rwlock, default_rwlock_init);
	return pthread_rwlock_rdlock(rwlock->glibc);
}

int bionic_pthread_rwlock_unlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	INIT_IF_NOT_MAPPED(rwlock, default_rwlock_init);
	return pthread_rwlock_unlock(rwlock->glibc);
}

int bionic_pthread_rwlock_wrlock(bionic_rwlock_t *rwlock)
{
	assert(rwlock);
	INIT_IF_NOT_MAPPED(rwlock, default_rwlock_init);
	return pthread_rwlock_wrlock(rwlock->glibc);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* attr */

int bionic_pthread_attr_destroy(bionic_attr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_attr_destroy(attr->glibc);
		munmap(attr->glibc, sizeof(*attr->glibc));
	}
	return ret;
}

int bionic_pthread_attr_init(bionic_attr_t *attr)
{
	assert(attr);
	// From PTHREAD_ATTR_INIT(3)
	// Calling `pthread_attr_init` on a thread attributes object that has already been initialized results in ud.
	*attr = (bionic_attr_t){0};
	attr->glibc = mmap(NULL, sizeof(*attr->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_attr_init(attr->glibc);
}

int bionic_pthread_getattr_np(bionic_pthread_t thread, bionic_attr_t *attr)
{
	assert(thread && attr);
	*attr = (bionic_attr_t){0};
	attr->glibc = mmap(NULL, sizeof(*attr->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_getattr_np((pthread_t)thread, attr->glibc);
}

int bionic_pthread_attr_settstack(bionic_attr_t *attr, void *stackaddr, size_t stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setstack(attr->glibc, stackaddr, stacksize);
}

int bionic_pthread_attr_getstack(const bionic_attr_t *attr, void *stackaddr, size_t *stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getstack(attr->glibc, stackaddr, stacksize);
}

int bionic_pthread_attr_setstacksize(bionic_attr_t *attr, size_t stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setstacksize(attr->glibc, stacksize);
}

int bionic_pthread_attr_getstacksize(const bionic_attr_t *attr, size_t *stacksize)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getstacksize(attr->glibc, stacksize);
}

int bionic_pthread_attr_setschedpolicy(bionic_attr_t *attr, int policy)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setschedpolicy(attr->glibc, policy);
}

int bionic_pthread_attr_getschedpolicy(bionic_attr_t *attr, int *policy)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getschedpolicy(attr->glibc, policy);
}

int bionic_pthread_attr_setschedparam(bionic_attr_t *attr, const struct sched_param *param)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setschedparam(attr->glibc, param);
}

int bionic_pthread_attr_getschedparam(bionic_attr_t *attr, struct sched_param *param)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getschedparam(attr->glibc, param);
}

int bionic_pthread_attr_setdetachstate(bionic_attr_t *attr, int detachstate)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_setdetachstate(attr->glibc, detachstate);
}

int bionic_pthread_attr_getdetachstate(bionic_attr_t *attr, int *detachstate)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_attr_getdetachstate(attr->glibc, detachstate);
}

int bionic_pthread_create(bionic_pthread_t *thread, const bionic_attr_t *attr, void* (*start)(void*), void *arg)
{
	assert(thread && (!attr || IS_MAPPED(attr)));
	return pthread_create((pthread_t*)thread, (attr ? attr->glibc : NULL), start, arg);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* mutexattr */

int bionic_pthread_mutexattr_settype(bionic_mutexattr_t *attr, int type)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_mutexattr_settype(attr->glibc, type);
}

int bionic_pthread_mutexattr_destroy(bionic_mutexattr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_mutexattr_destroy(attr->glibc);
		munmap(attr->glibc, sizeof(*attr->glibc));
	}
	return ret;
}

int bionic_pthread_mutexattr_init(bionic_mutexattr_t *attr)
{
	assert(attr);
	// From PTHREAD_MUTEXATTR_INIT(3)
	// The results of initializing an already initialized mutex attributes object are undefined.
	*attr = (bionic_mutexattr_t){0};
	attr->glibc = mmap(NULL, sizeof(*attr->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_mutexattr_init(attr->glibc);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* mutex */

static void default_pthread_mutex_init(bionic_mutex_t *mutex)
{
	assert(mutex);

	for (size_t i = 0; i < ARRAY_SIZE(bionic_mutex_init_map); i++) {
		if (memcmp(&bionic_mutex_init_map[i].bionic, mutex, sizeof(*mutex)))
			continue;

		mutex->glibc = mmap(NULL, sizeof(*mutex->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		memcpy(mutex->glibc, &bionic_mutex_init_map[i].glibc, sizeof(bionic_mutex_init_map[i].glibc));
		return;
	}

	assert(0 && "no such default initializer???");
}

int bionic_pthread_mutex_destroy(bionic_mutex_t *mutex)
{
	assert(mutex);
	int ret = 0;
	if (IS_MAPPED(mutex)) {
		ret = pthread_mutex_destroy(mutex->glibc);
		munmap(mutex->glibc, sizeof(*mutex->glibc));
	}
	return ret;
}

int bionic_pthread_mutex_init(bionic_mutex_t *mutex, const bionic_mutexattr_t *attr)
{
	assert(mutex && (!attr || IS_MAPPED(attr)));
	// From PTHREAD_MUTEX_INIT(3)
	// Attempting to initialize an already initialized mutex result in undefined behavior.
	*mutex = (bionic_mutex_t){0};
	mutex->glibc = mmap(NULL, sizeof(*mutex->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_mutex_init(mutex->glibc, (attr ? attr->glibc : NULL));
}

int
bionic_pthread_mutex_lock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_mutex_lock(mutex->glibc);
}

int bionic_pthread_mutex_trylock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_mutex_trylock(mutex->glibc);
}

int bionic_pthread_mutex_unlock(bionic_mutex_t *mutex)
{
	assert(mutex);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_mutex_unlock(mutex->glibc);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* condattr */

int bionic_pthread_condattr_destroy(bionic_condattr_t *attr)
{
	assert(attr);
	int ret = 0;
	if (IS_MAPPED(attr)) {
		ret = pthread_condattr_destroy(attr->glibc);
		munmap(attr->glibc, sizeof(*attr->glibc));
	}
	return ret;
}

int bionic_pthread_condattr_init(bionic_condattr_t *attr)
{
	*attr = (bionic_condattr_t){0};
	attr->glibc = mmap(NULL, sizeof(*attr->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_condattr_init(attr->glibc);
}

int bionic_pthread_condattr_setclock(bionic_condattr_t *attr, clockid_t clock_id)
{
	assert(attr && IS_MAPPED(attr));
	return pthread_condattr_setclock(attr->glibc, clock_id);
}

/* ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- *
 * ---------------------------------------------------------------------------------------------- */

/* cond */

static void default_pthread_cond_init(bionic_cond_t *cond)
{
	assert(cond);
	cond->glibc = mmap(NULL, sizeof(*cond->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(cond->glibc, 0, sizeof(*cond->glibc));
}

int bionic_pthread_cond_destroy(bionic_cond_t *cond)
{
	assert(cond);
	int ret = 0;
	if (IS_MAPPED(cond)) {
		ret = pthread_cond_destroy(cond->glibc);
		munmap(cond->glibc, sizeof(*cond->glibc));
	}
	return ret;
}

int bionic_pthread_cond_init(bionic_cond_t *cond, const bionic_condattr_t *attr)
{
	// SUS // assert(cond && (!attr || IS_MAPPED(attr)));
	// From PTHREAD_COND_INIT(3)
	// Attempting to initialize an already initialized mutex result in undefined behavior.
	*cond = (bionic_cond_t){0};
	cond->glibc = mmap(NULL, sizeof(*cond->glibc), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return pthread_cond_init(cond->glibc, (attr ? attr->glibc : NULL));
}

int bionic_pthread_cond_broadcast(bionic_cond_t *cond)
{
	assert(cond);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	return pthread_cond_broadcast(cond->glibc);
}

int bionic_pthread_cond_signal(bionic_cond_t *cond)
{
	assert(cond);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	return pthread_cond_signal(cond->glibc);
}

int
bionic_pthread_cond_wait(bionic_cond_t *cond, bionic_mutex_t *mutex) {
	assert(cond && mutex);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_cond_wait(cond->glibc, mutex->glibc);
}

int bionic_pthread_cond_timedwait(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abs_timeout)
{
	assert(cond && mutex);
	INIT_IF_NOT_MAPPED(cond, default_pthread_cond_init);
	INIT_IF_NOT_MAPPED(mutex, default_pthread_mutex_init);
	return pthread_cond_timedwait(cond->glibc, mutex->glibc, abs_timeout);
}

int bionic_pthread_cond_timedwait_relative_np(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *reltime)
{
	assert(cond && mutex && reltime);
	struct timespec tv;
	clock_gettime(CLOCK_REALTIME, &tv);
	tv.tv_sec += reltime->tv_sec;
	tv.tv_nsec += reltime->tv_nsec;
	if (tv.tv_nsec >= 1000000000) {
		++tv.tv_sec;
		tv.tv_nsec -= 1000000000;
	}
	return bionic_pthread_cond_timedwait(cond, mutex, &tv);
}

int bionic_pthread_cond_timedwait_monotonic_np(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abstime)
{
	assert(cond && mutex && abstime);
	struct timespec tv;
	clock_gettime(CLOCK_MONOTONIC, &tv);
	tv.tv_sec += abstime->tv_sec;
	tv.tv_nsec += abstime->tv_nsec;
	if (tv.tv_nsec >= 1000000000) {
		++tv.tv_sec;
		tv.tv_nsec -= 1000000000;
	}
	return bionic_pthread_cond_timedwait(cond, mutex, &tv);
}

int bionic_pthread_cond_timedwait_monotonic(bionic_cond_t *cond, bionic_mutex_t *mutex, const struct timespec *abstime)
{
	return bionic_pthread_cond_timedwait_monotonic_np(cond, mutex, abstime);
}
