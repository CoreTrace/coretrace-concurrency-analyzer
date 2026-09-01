// SPDX-License-Identifier: Apache-2.0
// Regression: the two workers guard the same field with *different* sibling mutexes, so the
// field is unprotected. Collapsing every member of a global aggregate onto the base symbol
// made both locks look identical and suppressed the race.
#include <pthread.h>

struct guarded_state
{
    pthread_mutex_t first_lock;
    pthread_mutex_t second_lock;
    int value;
};

struct guarded_state state = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, 0};

void* increment_under_first(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&state.first_lock);
    state.value++;
    pthread_mutex_unlock(&state.first_lock);
    return 0;
}

void* increment_under_second(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&state.second_lock);
    state.value++;
    pthread_mutex_unlock(&state.second_lock);
    return 0;
}

int main(void)
{
    pthread_t first, second;
    pthread_create(&first, 0, increment_under_first, 0);
    pthread_create(&second, 0, increment_under_second, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
    return state.value;
}
