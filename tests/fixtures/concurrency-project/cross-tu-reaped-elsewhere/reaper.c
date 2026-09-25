// SPDX-License-Identifier: Apache-2.0
// Reaping unit: it waits for the child the other unit forked.
#include <sys/types.h>
#include <sys/wait.h>

void collect_child(pid_t child)
{
    int status = 0;
    waitpid(child, &status, 0);
}
