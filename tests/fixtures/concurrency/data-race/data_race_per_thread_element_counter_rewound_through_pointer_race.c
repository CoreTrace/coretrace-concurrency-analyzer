// SPDX-License-Identifier: Apache-2.0
// A pointer to the counter rewinds it once, after the second round: results[0] and results[1]
// are each handed to two threads (#108).
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
    int i = 0;
    int* counter = &i;
    for (; i < 4; ++i)
    {
        pthread_create(&threads[i + 2 * rewound], NULL, worker, &results[i]);
        if (i == 1 && !rewound)
        {
            rewound = 1;
            *counter = -1;
        }
    }
    return 0;
}
