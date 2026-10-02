// SPDX-License-Identifier: Apache-2.0
// main writes between the spawn loop and the join loop, then inside the join loop, while later
// threads still run; it writes once more after every thread is joined (#113).
// Expected: two data races, between and injoin; none on after.
#include <pthread.h>
static int between;
static int injoin;
static int after;
static void* reader(void* argument)
{
    (void)argument;
    return (void*)(long)(between + injoin + after);
}
int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&threads[i], NULL, reader, NULL);
    between = 1;
    for (int i = 0; i < 4; ++i)
    {
        pthread_join(threads[i], NULL);
        injoin = i;
    }
    after = 1;
    return 0;
}
