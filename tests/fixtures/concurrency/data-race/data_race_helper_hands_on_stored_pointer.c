// SPDX-License-Identifier: Apache-2.0
// Both threads call a helper that hands a pointer stored in the shared structure to a second
// helper, which writes through it. The written node is reached only through the structure, and
// no access the second helper makes is carried back through that pointer, so the call still
// covers the structure and the race stays reported.
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

static void bump(struct Node* node)
{
    node->value += 1;
}

static void bump_head(struct List* target)
{
    bump(target->head);
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
