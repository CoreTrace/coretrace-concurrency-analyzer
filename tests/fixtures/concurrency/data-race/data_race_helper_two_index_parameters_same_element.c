// SPDX-License-Identifier: Apache-2.0
// bump() increments grid[row][column]: two parameters pick the element, and main calls bump(1, 1)
// while the thread increments grid[1][1] itself. With two indices the element stays unknown, so
// the race stays reported (#159).
// Expected: one data race on `grid`, main's call to bump() against the thread's increment.
#include <pthread.h>
#include <stddef.h>

static int grid[2][2];

static void bump(int row, int column)
{
    grid[row][column] += 1;
}

static void* worker(void* argument)
{
    (void)argument;
    grid[1][1] += 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bump(1, 1);
    pthread_join(thread, NULL);
    return grid[1][1];
}
