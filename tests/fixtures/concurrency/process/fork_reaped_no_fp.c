// SPDX-License-Identifier: Apache-2.0
// The correct counterpart: no thread, and every child is collected.
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    pid_t children[4];

    for (int i = 0; i < 4; ++i)
    {
        children[i] = fork();
        if (children[i] == 0)
            return 0;
    }

    for (int i = 0; i < 4; ++i)
        waitpid(children[i], NULL, 0);

    return 0;
}
