// SPDX-License-Identifier: Apache-2.0
// Right after raising the counter, the round rewinds it once to 0: results[0] and results[1] are
// each handed to two threads (#108).
// Nothing joins the threads: their join loop would not be matched (#173).
// Expected: one data race and one missing join.
#include <pthread.h>

static int results[4];

static void* worker(void* argument)
{
    int* element = argument;
    *element = 42;
    return NULL;
}

int main(void)
{
    pthread_t threads[6];
    int rewound = 0;
    int started = 0;
    int i = 0;
    while (i < 4)
    {
        pthread_create(&threads[started++], NULL, worker, &results[i]);
        ++i;
        if (i == 2 && !rewound)
        {
            rewound = 1;
            i = 0;
        }
    }
    return 0;
}
