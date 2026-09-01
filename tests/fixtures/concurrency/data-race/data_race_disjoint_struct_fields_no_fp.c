// SPDX-License-Identifier: Apache-2.0
// Regression: two workers write distinct fields of the same global struct, which are
// distinct memory locations in C.
#include <pthread.h>

struct pair
{
    int first;
    int second;
};

struct pair shared_pair;

void* write_first(void* arg)
{
    (void)arg;
    shared_pair.first = 1;
    return 0;
}

void* write_second(void* arg)
{
    (void)arg;
    shared_pair.second = 2;
    return 0;
}

int main(void)
{
    pthread_t first, second;
    pthread_create(&first, 0, write_first, 0);
    pthread_create(&second, 0, write_second, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
    return 0;
}
