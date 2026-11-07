// SPDX-License-Identifier: Apache-2.0
// Starts writer, which main.c defines and starts on task, as a thread on other: under other's lock,
// which excludes neither main.c's thread nor reader.
// Expected in project mode with main.c: two data races on shared, between the two runs of
// writer and between reader and writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

extern struct Task other;
void* writer(void* argument);

void start_other(void)
{
    pthread_create(&other.thread, NULL, writer, &other);
}

void join_other(void)
{
    pthread_join(other.thread, NULL);
}
