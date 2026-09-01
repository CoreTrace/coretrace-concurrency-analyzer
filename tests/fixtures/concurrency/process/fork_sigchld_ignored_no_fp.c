// SPDX-License-Identifier: Apache-2.0
// Handing SIGCHLD to SIG_IGN asks the system to reap children itself, so none of them can
// become a zombie. It is the usual way a program that never reads an exit status stays clean,
// and it owes no wait.
#include <signal.h>
#include <stddef.h>
#include <unistd.h>

int main(void)
{
    signal(SIGCHLD, SIG_IGN);

    for (int i = 0; i < 4; ++i)
    {
        if (fork() == 0)
            return 0;
    }

    return 0;
}
