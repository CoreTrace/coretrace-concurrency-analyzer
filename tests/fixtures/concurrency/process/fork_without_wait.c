// SPDX-License-Identifier: Apache-2.0
// No thread here, so the fork itself is fine. The pid is dropped on the floor: every child that
// finishes stays in the process table until this process exits.
#include <stddef.h>
#include <unistd.h>

int main(void)
{
    for (int i = 0; i < 4; ++i)
    {
        if (fork() == 0)
            return 0;
    }

    return 0;
}
