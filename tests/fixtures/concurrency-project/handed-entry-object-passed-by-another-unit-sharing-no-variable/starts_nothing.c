// SPDX-License-Identifier: Apache-2.0
// Shares no variable with main.c and refers to nothing it defines: neither writer, the entry, nor
// startWriter, the function passing writer's helper an object. Every run of writer is the thread
// main.c's helper starts on main's own task, under task's lock, as reader holds it.
// Expected in project mode with main.c: no data race.
#include <pthread.h>

void start_other(void)
{
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);
}

void join_other(void) {}
