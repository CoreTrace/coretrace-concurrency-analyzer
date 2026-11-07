// SPDX-License-Identifier: Apache-2.0
// Calls startWriter, which main.c defines, on other: the thread it starts on writer holds other's
// lock, which excludes nothing reader holds.
// Expected in project mode with main.c: one data race on shared, between reader and writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

extern struct Task other;
void startWriter(struct Task* started);

void start_other(void)
{
    startWriter(&other);
}

void join_other(void)
{
    pthread_join(other.thread, NULL);
}
