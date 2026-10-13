// SPDX-License-Identifier: Apache-2.0
// The helper joins the thread main started before starting the next one in another field: the
// two never run at once, and main joins the second before returning (#123).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

static int shared;

struct Task
{
    pthread_t first;
    pthread_t second;
};

static void* run(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void hand_over(struct Task* task)
{
    pthread_join(task->first, NULL);
    pthread_create(&task->second, NULL, run, NULL);
}

int main(void)
{
    struct Task task;
    pthread_create(&task.first, NULL, run, NULL);
    hand_over(&task);
    pthread_join(task.second, NULL);
    return shared;
}
