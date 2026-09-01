// SPDX-License-Identifier: Apache-2.0
// The correct shape: the handler writes a flag of the one type the standard guarantees is safe
// to touch there, and every decision is taken outside it.
#include <signal.h>
#include <stddef.h>

static volatile sig_atomic_t interrupted = 0;

static void on_interrupt(int received)
{
    (void)received;
    interrupted = 1;
}

int main(void)
{
    struct sigaction action;
    action.sa_handler = on_interrupt;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);

    while (!interrupted)
        ;

    return 0;
}
