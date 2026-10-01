// SPDX-License-Identifier: Apache-2.0
// scan() hands step() the distance from the start of `text` to its cursor, and step() increments
// counts[index], then scans again from the next character. main scans from text[0] and the thread
// from text[2]: both reach counts[2] and counts[3]. scan() takes only a pointer the analysis
// cannot name, so no call of it passes anything an access could wait on: around the recursion the
// accesses stay those of scan(), at an unknown place, and the race is reported once (#159).
// Expected: one data race on `counts`, main's scan against the thread's.
#include <pthread.h>
#include <stddef.h>

static int counts[4];
static const char text[] = "abcd";

static void step(int index);

static void scan(const char* cursor)
{
    if (*cursor != '\0')
        step((int)(cursor - text));
}

static void step(int index)
{
    counts[index] += 1;
    scan(text + index + 1);
}

static void* worker(void* argument)
{
    (void)argument;
    scan(text + 2);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    scan(text);
    pthread_join(thread, NULL);
    return counts[3];
}
