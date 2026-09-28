// SPDX-License-Identifier: Apache-2.0
// Both threads call a helper that only reads through a pointer stored in the shared structure.
// The node read is reached through the structure, so the call covers the structure, but as a
// read: two reads never race.
#include <pthread.h>
#include <stddef.h>

struct Node
{
    int value;
};

struct List
{
    struct Node* head;
};

static struct Node first = {1};
static struct List list = {&first};

static int head_value(const struct List* target)
{
    return target->head->value;
}

static void* worker(void* argument)
{
    (void)argument;
    return (void*)(size_t)head_value(&list);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    int result = head_value(&list);
    pthread_join(thread, NULL);
    return result;
}
