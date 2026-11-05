// SPDX-License-Identifier: Apache-2.0
// Takes other's lock and refers to nothing of main.c's entry: every run of writer is the thread
// main.c's helper starts on task, under task's lock, as reader holds it.
// Expected in project mode with main.c: no data race.
#include <pthread.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

extern struct Task other;

void run_other(void)
{
    pthread_mutex_lock(&other.lock);
    pthread_mutex_unlock(&other.lock);
}
