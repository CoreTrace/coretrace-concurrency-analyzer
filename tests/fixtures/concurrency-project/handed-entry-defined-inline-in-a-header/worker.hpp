// SPDX-License-Identifier: Apache-2.0
// The entry, defined inline: every unit using it defines its own copy, and none declares it.
#pragma once

#include <pthread.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

extern int shared;

inline void* writer(void* argument)
{
    Task* held = static_cast<Task*>(argument);
    pthread_mutex_lock(&held->lock);
    shared += 1;
    pthread_mutex_unlock(&held->lock);
    return nullptr;
}
