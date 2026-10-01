// SPDX-License-Identifier: Apache-2.0
// `worker` is started on &a and joined before anything else runs. Then main and `other` both call
// write_b(), which calls worker(&b): the two calls write `b` at the same time, and nothing but the
// joined thread writes `a`. The thread's access to `a` is also copied to the call in write_b(),
// on the object the spawn passed (#156); that copy is still the thread's alone, and must not be
// taken for write_b()'s own access, run by main and `other` at once (#155).
// Expected: one data race on `b`, main's call to write_b() against `other`'s.
#include <pthread.h>
#include <stddef.h>

static int a;
static int b;

static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}

static void write_b(void)
{
    worker(&b);
}

static void* other(void* argument)
{
    (void)argument;
    write_b();
    return NULL;
}

int main(void)
{
    pthread_t workerThread;
    pthread_t otherThread;
    pthread_create(&workerThread, NULL, worker, &a);
    pthread_join(workerThread, NULL);
    pthread_create(&otherThread, NULL, other, NULL);
    write_b();
    pthread_join(otherThread, NULL);
    return a + b;
}
