// SPDX-License-Identifier: Apache-2.0
// One local holds two objects, each with its handle at the same place. The restarting callee is
// handed both: it first joins the thread of the other object, then on a path that exits overwrites
// the handle of the thread main started and calls a helper joining it on some of its paths only.
// The join made through the other parameter waited for another thread: the helper's write runs
// beside the thread main started (#201).
// Expected: the write in maybe_join races with run. The race of run with itself, false (#256), and
// the missing join of the spare thread, joined through its copied handle (#255), are not pinned.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Task
{
    int value;
    pthread_t thread;
};

struct Pair
{
    struct Task first;
    struct Task second;
};

static void* run(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void maybe_join(struct Task* task, int really)
{
    if (!really)
        return;
    pthread_join(task->thread, NULL);
    task->value += 1;
}

static void restart(struct Task* task, struct Task* other, int fatal)
{
    pthread_join(other->thread, NULL);
    if (fatal)
    {
        pthread_t spare;
        pthread_create(&spare, NULL, idle, NULL);
        task->thread = spare;
        maybe_join(task, 1);
        exit(1);
    }
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Pair pair = {0};
    pthread_create(&pair.second.thread, NULL, idle, NULL);
    pthread_create(&pair.first.thread, NULL, run, &pair.first);
    restart(&pair.first, &pair.second, argc > 1);
    pthread_join(pair.first.thread, NULL);
    return pair.first.value;
}
