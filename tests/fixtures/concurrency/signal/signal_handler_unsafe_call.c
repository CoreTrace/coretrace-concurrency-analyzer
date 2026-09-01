// SPDX-License-Identifier: Apache-2.0
// The handler prints and allocates. Either can interrupt the same operation in the main flow:
// stdio keeps a lock the interrupted code may already hold, and the allocator keeps a free list
// it may be halfway through rewiring.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static void on_interrupt(int received)
{
    (void)received;
    printf("interrupted\n");
    free(malloc(8));
}

int main(void)
{
    signal(SIGINT, on_interrupt);
    return 0;
}
