// SPDX-License-Identifier: Apache-2.0
// Includes worker.hpp, which defines writer inline, without using writer: this unit defines no
// copy of it and runs it nowhere, which leaves writer bound to main's task.
// Expected in project mode with main.cpp: no data race.
#include "worker.hpp"

#include <pthread.h>

void run_other()
{
    Task mine = {PTHREAD_MUTEX_INITIALIZER, {}};
    pthread_mutex_lock(&mine.lock);
    pthread_mutex_unlock(&mine.lock);
}
