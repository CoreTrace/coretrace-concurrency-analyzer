// SPDX-License-Identifier: Apache-2.0
// The handler itself looks harmless; the lock is one call away. A handler is no safer than what
// it calls, and this mutex may already be held by the very thread the signal interrupted.
#include <pthread.h>
#include <signal.h>

static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static int pending = 0;

static void record_signal(void)
{
    pthread_mutex_lock(&guard);
    pending = pending + 1;
    pthread_mutex_unlock(&guard);
}

static void on_interrupt(int received)
{
    (void)received;
    record_signal();
}

int main(void)
{
    signal(SIGINT, on_interrupt);
    return 0;
}
