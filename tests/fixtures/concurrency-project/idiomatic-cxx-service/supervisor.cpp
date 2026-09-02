// SPDX-License-Identifier: Apache-2.0
#include "supervisor.hpp"

#include "worker.hpp"

#include <unistd.h>

void spawn_children(std::size_t count)
{
    // A worker runs in this process before the first fork, so each child inherits its mutexes
    // without the thread that may hold them. No exec follows, and no pid is ever collected.
    Worker local_worker;

    for (std::size_t index = 0; index < count; ++index)
    {
        if (fork() == 0)
        {
            Worker child_worker;
            return;
        }
    }
}
