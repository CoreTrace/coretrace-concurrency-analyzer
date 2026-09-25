// SPDX-License-Identifier: Apache-2.0
// Forking unit: it hands the child's pid to a helper defined in another unit. On its own it
// never waits, and saying so is right — this unit has no way to know the helper reaps.
#include <sys/types.h>
#include <unistd.h>

void collect_child(pid_t child);

int main(void)
{
    const pid_t child = fork();
    if (child == 0)
        _exit(0);

    collect_child(child);
    return 0;
}
