// SPDX-License-Identifier: Apache-2.0
// The safe counterpart of calling strtok from a handler: strtok_r keeps its position in a
// pointer the caller owns, so the handler's scan cannot disturb the one it interrupted. POSIX
// lists strtok_r among the async-signal-safe functions and leaves strtok out.
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t fields = 0;

static void on_usr1(int received)
{
    (void)received;
    char text[] = "a:b:c";
    char* position = NULL;
    for (char* field = strtok_r(text, ":", &position); field != NULL;
         field = strtok_r(NULL, ":", &position))
        ++fields;
}

int main(void)
{
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    return fields == 3 ? 0 : 1;
}
