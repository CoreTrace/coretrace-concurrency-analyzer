// SPDX-License-Identifier: Apache-2.0
// The helper joins the first thread it starts on the handed entry before starting the second:
// the two never run at once (#123).
// Expected: no data race.
#include <pthread.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return 0;
}

static void run(void* (*entry)(void*))
{
    pthread_t first;
    pthread_t second;
    pthread_create(&first, 0, entry, 0);
    pthread_join(first, 0);
    pthread_create(&second, 0, entry, 0);
    pthread_join(second, 0);
}

int main(void)
{
    run(worker);
    return shared;
}
