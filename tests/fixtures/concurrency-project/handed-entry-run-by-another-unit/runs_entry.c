// SPDX-License-Identifier: Apache-2.0
// Runs writer, which main.c defines and starts on task, on other: under other's lock, which
// excludes nothing reader holds.
// Expected in project mode with main.c: one data race on shared, between reader and writer.
#include <pthread.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

extern struct Task other;
void* writer(void* argument);

void run_other(void)
{
    writer(&other);
}
