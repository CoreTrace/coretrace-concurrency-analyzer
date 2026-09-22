// SPDX-License-Identifier: Apache-2.0
/* Cause (b): spawn and join both live in loops of ONE function. A loop-body join never
 * dominates the code after the loop, so phase A is still considered live when phase B starts.
 * Expected today: a race pairing workerA with workerB, although they never coexist. */
#include <pthread.h>
int g_shared = 0;
extern int stop;
static void* workerA(void* a) { (void)a; g_shared++; return 0; }
static void* workerB(void* a) { (void)a; g_shared++; return 0; }
int main(void) {
    pthread_t a[2], b[2];
    for (int i = 0; i < 2; ++i) pthread_create(&a[i], 0, workerA, 0);
    for (int i = 0; i < 2; ++i) { pthread_join(a[i], 0); if (stop) break; }
    for (int i = 0; i < 2; ++i) pthread_create(&b[i], 0, workerB, 0);
    for (int i = 0; i < 2; ++i) pthread_join(b[i], 0);
    return g_shared;
}
