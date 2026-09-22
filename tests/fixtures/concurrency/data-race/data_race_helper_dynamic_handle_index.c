// SPDX-License-Identifier: Apache-2.0
#include <pthread.h>
int g_shared = 0;
static void* workerA(void* a) { (void)a; g_shared++; return 0; }
static void* workerB(void* a) { (void)a; g_shared++; return 0; }
static void runPhase(void* (*entry)(void*)) {
    pthread_t h[2];
    extern unsigned create_index, join_index;
    pthread_create(&h[create_index], 0, entry, 0);
    pthread_join(h[join_index], 0);              /* joined before returning */
}
int main(void) {
    runPhase(workerA);               /* phase 1 */
    runPhase(workerB);               /* phase 2: cannot overlap phase 1 */
    return g_shared;
}
