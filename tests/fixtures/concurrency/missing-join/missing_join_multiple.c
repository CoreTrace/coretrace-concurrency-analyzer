// SPDX-License-Identifier: Apache-2.0
// Test: Missing join multiple - plusieurs threads non joints
#include <pthread.h>
#include <stdio.h>

int results[5] = {0};

void* compute(void* arg) {
    int id = *(int*)arg;
    // Simulation de calcul
    for (volatile int i = 0; i < 10000; i++);
    results[id] = id * 10;
    printf("Thread %d completed\n", id);
    return NULL;
}

int main() {
    pthread_t threads[5];
    int ids[5] = {0, 1, 2, 3, 4};
    
    // Création de 5 threads
    for (int i = 0; i < 5; i++) {
        pthread_create(&threads[i], NULL, compute, &ids[i]);
    }
    
    // MISSING JOIN: seul le premier thread est joint
    pthread_join(threads[0], NULL);
    
    // Les 4 autres threads ne sont jamais joints!
    // Leurs résultats peuvent être incomplets ou perdus
    
    printf("Results: ");
    for (int i = 0; i < 5; i++) {
        printf("%d ", results[i]);
    }
    printf("\n");
    
    return 0;
}

// Known M1 precision limit: the analyzer is array-element-insensitive and
// therefore reports concurrent writes on the global array as a single race on
// `results`, even though each worker targets a distinct index.
// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: compute
// 	severity: ERROR
// 	ruleId: DataRaceGlobal
// 	cwe: CWE-362
// 	symbol: results
// 	at line 12, column 17
// 	[!!!Error] unsynchronized concurrent access to global 'results'
// 	     ↳ access: write at ${REPO_ROOT}/tests/fixtures/concurrency/missing-join/missing_join_multiple.c:12:17 in compute (thread entries: compute)
// 	     ↳ conflicts with another concurrent invocation reachable from thread entry 'compute'
// 	     ↳ possible conflict kinds: write/write
// 	     ↳ no common recognized lock protects the conflicting accesses
// 	related: Concurrent invocation -> ${REPO_ROOT}/tests/fixtures/concurrency/missing-join/missing_join_multiple.c:12:17 in compute
//
// Function: main
// 	severity: WARNING
// 	ruleId: MissingJoin
// 	at line 23, column 9
// 	[!!!Warning] thread handle is not joined or detached before scope exit
// 	     ↳ handle kind: pthread
// 	     ↳ lifecycle summary: creates=1, joins=0, detaches=0
// 	     ↳ outstanding joinable handles: 1
// EXPECT-HUMAN-DIAGNOSTICS-END
