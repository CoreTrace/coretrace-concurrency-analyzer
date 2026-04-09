// SPDX-License-Identifier: Apache-2.0
// Test 1: Data race basique - écriture concurrente sans synchronisation
#include <pthread.h>
#include <stdio.h>

int shared_counter = 0;  // Variable partagée non protégée

void* increment(void* arg) {
    for (int i = 0; i < 10000; i++) {
        shared_counter++;  // DATA RACE: lecture-modification-écriture non atomique
    }
    return NULL;
}

int main() {
    pthread_t t1, t2;

    pthread_create(&t1, NULL, increment, NULL);
    pthread_create(&t2, NULL, increment, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("Counter: %d (attendu: 20000)\n", shared_counter);
    return 0;
}

// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: increment
// 	severity: ERROR
// 	ruleId: DataRaceGlobal
// 	cwe: CWE-362
// 	symbol: shared_counter
// 	at line 10, column 23
// 	[!!!Error] unsynchronized concurrent access to global 'shared_counter'
// 	     ↳ first access: read at ${REPO_ROOT}/tests/fixtures/concurrency/data-race/data_race_basic.c:10:23 in increment (thread entries: increment)
// 	     ↳ conflicting access: write at ${REPO_ROOT}/tests/fixtures/concurrency/data-race/data_race_basic.c:10:23 in increment (thread entries: increment)
// 	     ↳ possible conflict kinds: read/write, write/write
// 	     ↳ no common recognized lock protects the conflicting accesses
// 	related: Conflicting access -> ${REPO_ROOT}/tests/fixtures/concurrency/data-race/data_race_basic.c:10:23 in increment
// EXPECT-HUMAN-DIAGNOSTICS-END
