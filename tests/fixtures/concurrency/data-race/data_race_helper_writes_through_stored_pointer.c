// SPDX-License-Identifier: Apache-2.0
// Both threads call a helper that writes through a pointer stored in the shared structure. The
// written node is not the structure itself, but the helper reaches it only through the structure,
// so what the call may do covers the structure as a whole and the race stays reported.
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

static struct Node first;
static struct List list = {&first};

static void bump_head(struct List* target)
{
    target->head->value += 1;
}

static void* worker(void* argument)
{
    (void)argument;
    bump_head(&list);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bump_head(&list);
    pthread_join(thread, NULL);
    return first.value;
}
