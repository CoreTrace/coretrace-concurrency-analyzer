// SPDX-License-Identifier: Apache-2.0
// A recursion hands its own pointer on unchanged and bumps one field of the object, while a
// thread writes another field. The pointer does not move around the cycle, so each access keeps
// the field it touches, and the two fields do not race (#117).
#include <pthread.h>
#include <stddef.h>

struct Stats
{
    int visits;
    int errors;
};

static struct Stats stats;

static void visit(struct Stats* target, int remaining)
{
    target->visits += 1;
    if (remaining > 1)
        visit(target, remaining - 1);
}

static void* recordError(void* argument)
{
    (void)argument;
    stats.errors += 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, recordError, NULL);
    visit(&stats, 4);
    pthread_join(thread, NULL);
    return stats.visits;
}
