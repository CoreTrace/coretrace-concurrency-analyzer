// SPDX-License-Identifier: Apache-2.0
#include <pthread.h>
int g_shared = 0;
static void* workerA(void* a) { (void)a; g_shared++; return 0; }
static void* workerB(void* a) { (void)a; g_shared++; return 0; }
static void runPhase(void* (*entry)(void*)) {
    pthread_t h;
    pthread_create(&h, 0, entry, 0);
    pthread_join(h, 0);              /* joined before returning */
}
static void outer(void* (*entry)(void*)) { runPhase(entry); }
int main(void) {
    outer(workerA);               /* phase 1 */
    outer(workerB);               /* phase 2: cannot overlap phase 1 */
    return g_shared;
}
