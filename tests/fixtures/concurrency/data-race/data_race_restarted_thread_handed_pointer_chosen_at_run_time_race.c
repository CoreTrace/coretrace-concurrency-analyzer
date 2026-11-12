// SPDX-License-Identifier: Apache-2.0
// A helper joins the thread main started, then starts rewriting on a pointer a condition picks
// between the object holding the handle and another one: rewriting may replace the handle, so
// main's join may wait for idle while rewriting still writes shared (#201). The race on the
// handle itself is not reported: the object rewriting is handed is not known.
// Expected: one data race, main against rewriting.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t spare;
};

static int shared;

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* run(void* argument)
{
    (void)argument;
    return NULL;
}

static void* rewriting(void* argument)
{
    struct Task* task = argument;
    task->thread = task->spare;
    shared += 1;
    return NULL;
}

static void restart(struct Task* task, struct Task* other, int pick)
{
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, rewriting, pick ? task : other);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    struct Task other = {0};
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task, &other, argc > 1);
    pthread_join(task.thread, NULL);
    int seen = shared;
    pthread_join(task.spare, NULL);
    return seen;
}
