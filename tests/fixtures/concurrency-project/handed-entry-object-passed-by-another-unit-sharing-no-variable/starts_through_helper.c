// SPDX-License-Identifier: Apache-2.0
// Calls startWriter, which main.c defines, on an object of its own: the thread it starts on
// writer holds that object's lock, which excludes nothing reader holds.
// Expected in project mode with main.c: one data race on shared, between reader and writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task mine = {PTHREAD_MUTEX_INITIALIZER, 0};

void startWriter(struct Task* started);

void start_other(void)
{
    startWriter(&mine);
}

void join_other(void)
{
    pthread_join(mine.thread, NULL);
}
