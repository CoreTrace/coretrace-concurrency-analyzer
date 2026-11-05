// SPDX-License-Identifier: Apache-2.0
// Workers run on the elements of nodes, and a loop hands each element to start, which writes it,
// then starts reader on the pointer it reads out of it: every element points at common, and the
// readers race with each other on it (#242). As start writes the element before reading the
// field, what the field holds is not proven, and each reader keeps its element for lack of proof:
// it is not taken to run on an element of its own, which would exempt the readers from racing
// with each other.
// Expected: the readers race with each other, and start's write of an element races with the
// worker on it. The readers' race is reported on the elements they are kept on, not on common:
// what is pinned is that the pair is not dropped, not that its object is right, nodes being a
// conservative attribution. The readers' races with start and with the workers on those elements
// are false, and are not pinned.
#include <pthread.h>
#include <stddef.h>

struct Node
{
    int value;
    struct Node* next;
};

static struct Node nodes[2];
static struct Node common;

static void* worker(void* argument)
{
    struct Node* node = argument;
    node->value += 1;
    return NULL;
}

static void* reader(void* argument)
{
    struct Node* node = argument;
    node->value += 2;
    return NULL;
}

static void start(struct Node* node, pthread_t* thread)
{
    node->value = 0;
    pthread_create(thread, NULL, reader, node->next);
}

int main(void)
{
    pthread_t workers[2];
    pthread_t readers[2];
    for (int i = 0; i < 2; ++i)
        nodes[i].next = &common;
    for (int i = 0; i < 2; ++i)
        pthread_create(&workers[i], NULL, worker, &nodes[i]);
    for (int i = 0; i < 2; ++i)
        start(&nodes[i], &readers[i]);
    for (int i = 0; i < 2; ++i)
    {
        pthread_join(workers[i], NULL);
        pthread_join(readers[i], NULL);
    }
    return common.value;
}
