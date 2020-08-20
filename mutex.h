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
#define MUTEX(name) \
    static pthread_mutex_t name; \
    static bool name##_initialized = false;

#define mutex_initialize(name) { if (! name##_initialized) { \
                                    int ret; ASSERT (!(ret = pthread_mutex_init (&name, 0)), "Error: pthread_mutex_init failed for %s: %s", #name, strerror (ret)); \
                                    name##_initialized = true;  \
                               } }

#define mutex_destroy(name) if (name##_initialized) { pthread_mutex_destroy (&name); name##_initialized = false; }

#define mutex_lock(m)   { int ret = pthread_mutex_lock (&m); \
                          ASSERT (!ret, "Error in %s: pthread_mutex_lock failed: %s", __FUNCTION__, strerror (ret)); }

#define mutex_unlock(m) { int ret = pthread_mutex_unlock (&m); \
                          ASSERT (!ret, "Error in %s: pthread_mutex_unlock failed: %s", __FUNCTION__, strerror (ret)); }

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
#define spin_initialize(name) { if (! name##_initialized) { \
                                   int ret; ASSERT (!(ret = pthread_spin_init (&name, PTHREAD_PROCESS_PRIVATE)), "Error: pthread_spin_init failed for %s: %s", #name, strerror (ret)); \
                                   name##_initialized = true;  \
                              } }
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
#define spin_lock(m)   { int ret = pthread_spin_lock (&m); \
                         ASSERT (!ret, "Error in %s: pthread_spin_lock failed: %s", __FUNCTION__, strerror (ret)); }

#define spin_unlock(m) { int ret = pthread_spin_unlock (&m); \
                         ASSERT (!ret, "Error in %s: pthread_spin_lock failed: %s", __FUNCTION__, strerror (ret)); }
#endif

#endif