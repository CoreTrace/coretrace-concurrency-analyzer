// SPDX-License-Identifier: Apache-2.0
// Runs writer, which main.c defines and starts on main's own task, on an object of its own:
// under that object's lock, which excludes nothing reader holds.
// Expected in project mode with main.c: one data race on shared, between reader and writer.
#include <pthread.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

void* writer(void* argument);

void run_other(void)
{
    struct Task mine = {PTHREAD_MUTEX_INITIALIZER, 0};
    writer(&mine);
}
