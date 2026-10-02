// SPDX-License-Identifier: Apache-2.0
// main fills slots[i].in before starting thread i; thread i reads its in and writes its out; main
// reads the outs after the join loop (#108).
// Expected: no diagnostic.
#include <pthread.h>
struct slot
{
    int in;
    int out;
};
static void* worker(void* argument)
{
    struct slot* mine = argument;
    mine->out = mine->in * 2;
    return NULL;
}
int main(void)
{
    struct slot slots[4];
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
    {
        slots[i].in = i;
        pthread_create(&threads[i], NULL, worker, &slots[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);
    return slots[0].out + slots[3].out;
}
