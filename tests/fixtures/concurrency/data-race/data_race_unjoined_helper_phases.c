// SPDX-License-Identifier: Apache-2.0
#include <pthread.h>
int g_shared = 0;
static void* workerA(void* a) { (void)a; g_shared++; return 0; }
static void* workerB(void* a) { (void)a; g_shared++; return 0; }
static void runPhase(void* (*entry)(void*)) {
    pthread_t h;
    pthread_create(&h, 0, entry, 0);
    /* no join */              /* joined before returning */
}
int main(void) {
    runPhase(workerA);               /* phase 1 */
    runPhase(workerB);               /* phase 2: cannot overlap phase 1 */
    return g_shared;
}
