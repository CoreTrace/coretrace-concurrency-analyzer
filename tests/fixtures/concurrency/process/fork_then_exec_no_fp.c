// SPDX-License-Identifier: Apache-2.0
// The same fork, made safe the way POSIX intends: the child replaces its image at once, so it
// keeps nothing it inherited. Neither fork rule applies here.
#include <pthread.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t helper;
    pthread_create(&helper, NULL, worker, NULL);

    const pid_t child = fork();
    if (child == 0)
    {
        execl("/bin/echo", "echo", "done", (char*)NULL);
        return 1;
    }

    waitpid(child, NULL, 0);
    pthread_join(helper, NULL);
    return 0;
}
