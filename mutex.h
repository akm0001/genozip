// ------------------------------------------------------------------
//   mutex.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef MUTEX_INCLUDED
#define MUTEX_INCLUDED

#ifndef _MSC_VER // Microsoft compiler
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <availability.h>
#ifdef __MAC_10_12
#include <os/lock.h>
#else // spinlock was available prior to 10.12
#include <libkern/OSAtomic.h>
#endif
#endif

// -----------
// mutex stuff
// -----------
typedef struct Mutex {
    pthread_mutex_t mutex;
    const char *name, *initialized, *lock_func;
} Mutex;

void mutex_initialize_do (MutexP mutex, const char *name, const char *func);
#define mutex_initialize(mutex) mutex_initialize_do (&mutex, #mutex, __FUNCTION__)

void mutex_destroy_do (MutexP mutex, const char *func);
#define mutex_destroy(mutex) mutex_destroy_do (&mutex, __FUNCTION__)

void mutex_lock_do (MutexP mutex, const char *func);
#define mutex_lock(mutex) mutex_lock_do (&mutex, __FUNCTION__)

void mutex_unlock_do (MutexP mutex, const char *func, uint32_t line);
#define mutex_unlock(mutex) mutex_unlock_do (&mutex, __FUNCTION__, __LINE__)

#define mutex_is_show(name) (flag.show_mutex && (flag.show_mutex==(char*)1 || !strncmp ((name), flag.show_mutex, 8))) // only 8 chars so we can catch all genome_muteces[%u]

// -----------
// spinlock stuff
// -----------

#ifdef __APPLE__
#ifdef __MAC_10_12
#define pthread_spinlock_t os_unfair_lock
#define spin_lock(m)   os_unfair_lock_lock(&m)
#define spin_unlock(m) os_unfair_lock_unlock(&m)
#define spin_initialize(name) { if (! name##_initialized) { name = OS_UNFAIR_LOCK_INIT; name##_initialized = true; } }
#define spin_destroy(name) name##_initialized = false; 

#else // spinlock was available prior to 10.12
#define pthread_spinlock_t OSSpinLock
#define spin_initialize(name) { if (! name##_initialized) { name = 0; name##_initialized = true; } }
#define spin_destroy(name) name##_initialized = false; 
#define spin_lock(m)   OSSpinLockLock(&m)
#define spin_unlock(m) OSSpinLockUnlock(&m)
#endif // __MAC_10_12

#else // not mac
#define spin_initialize(name) do { if (! name##_initialized) { \
                                   int ret; ASSERTE (!(ret = pthread_spin_init (&name, PTHREAD_PROCESS_PRIVATE)), "failed for %s: %s", #name, strerror (ret)); \
                                   name##_initialized = true;  \
                                 } } while (0)
#define spin_destroy(name) if (name##_initialized) { pthread_spin_destroy (&name); name##_initialized = false; }

#endif // __APPLE__

#define SPINLOCK(name) \
    static pthread_spinlock_t name; \
    static bool name##_initialized = false;

#ifdef __APPLE__
#ifdef __MAC_10_12
#else // spinlock was available prior to 10.12
#endif // __MAC_10_12

#else
#define spin_lock(m)   do { int ret = pthread_spin_lock (&m); \
                            ASSERTE (!ret, "pthread_spin_lock failed: %s", strerror (ret)); } while(0)

#define spin_unlock(m) do { int ret = pthread_spin_unlock (&m); \
                            ASSERTE (!ret, "pthread_spin_lock failed: %s", strerror (ret)); } while(0)
#endif

#endif