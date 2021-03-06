// ------------------------------------------------------------------
//   mutex.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "mutex.h"
#include "flags.h"

void mutex_initialize_do (Mutex *mutex, const char *name, const char *func)
{ 
    if (mutex->initialized) return;

    int ret = pthread_mutex_init (&mutex->mutex, 0); 
    ASSERTE (!ret, "pthread_mutex_init failed for %s from %s: %s", name, func, strerror (ret));

    mutex->name = name;
    mutex->initialized = func;
}

void mutex_destroy_do (Mutex *mutex, const char *func) 
{
    if (!mutex->initialized) return;
    ASSERTW (!mutex->lock_func, "Warning in mutex_destroy_do called from %s: mutex %s is locked", func, mutex->name);
    
    pthread_mutex_destroy (&mutex->mutex); 
    mutex->initialized = NULL; 
}

void mutex_lock_do (Mutex *mutex, const char *func)   
{ 
    ASSERTE (mutex->initialized, "called from %s: mutex not initialized", func);

    bool show = mutex_is_show (mutex->name);

    if (show) iprintf ("LOCKING : Mutex %s by thread %"PRIu64" %s\n", mutex->name, (uint64_t)pthread_self(), func);

    int ret = pthread_mutex_lock (&mutex->mutex); 
    ASSERTE (!ret, "called from %s by %"PRIu64": pthread_mutex_lock failed: %s", 
            func, (uint64_t)pthread_self(), strerror (ret)); 

    mutex->lock_func = func; // mutex->lock_func is protected by the mutex

    if (show) iprintf ("LOCKED  : Mutex %s by thread %"PRIu64"\n", mutex->name, (uint64_t)pthread_self());

}

void mutex_unlock_do (Mutex *mutex, const char *func, uint32_t line) 
{ 
    ASSERTE (mutex->initialized, "called from %s:%u mutex not initialized", func, line);
    ASSERTE (mutex->lock_func, "called from %s:%u by thread=%"PRIu64": mutex %s is not locked", 
            func, line, (uint64_t)pthread_self(), mutex->name);

    mutex->lock_func = NULL; // mutex->lock_func is protected by the mutex

    int ret = pthread_mutex_unlock (&mutex->mutex); 
    ASSERTE (!ret, "called from %s:%u: pthread_mutex_unlock failed for %s: %s", func, line, mutex->name, strerror (ret)); 

    if (mutex_is_show (mutex->name))
        iprintf ("UNLOCKED: Mutex %s by thread %"PRIu64" %s\n", mutex->name, (uint64_t)pthread_self(), func);
}
