// SPDX-License-Identifier: Apache-2.0
// Test 11: Thread escape - fonction appelée parfois avec/sans thread
#include <pthread.h>
#include <stdio.h>

int shared_buffer[100];
int buffer_index = 0;  // Non protégé

// Cette fonction est appelée depuis un thread ET depuis main
void add_to_buffer(int value) {
    if (buffer_index < 100) {
        shared_buffer[buffer_index] = value;  // DATA RACE potentiel
        buffer_index++;
    }
}

void* thread_func(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 10; i++) {
        add_to_buffer(id * 10 + i);  // Appel depuis thread
    }
    return NULL;
}

int main() {
    pthread_t t;
    int thread_id = 1;
    
    pthread_create(&t, NULL, thread_func, &thread_id);
    
    // Main appelle aussi la fonction sans synchronisation!
    for (int i = 0; i < 10; i++) {
        add_to_buffer(i);  // Appel depuis main thread - DATA RACE!
    }
    
    pthread_join(t, NULL);
    
    printf("Buffer filled up to index: %d\n", buffer_index);
    return 0;
}

// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: add_to_buffer
// 	severity: ERROR
// 	ruleId: DataRaceGlobal
// 	cwe: CWE-362
// 	symbol: buffer_index
// 	at line 11, column 9
// 	[!!!Error] unsynchronized concurrent access to global 'buffer_index'
// 	     ↳ first access: read at ${REPO_ROOT}/tests/fixtures/concurrency/thread-escape/thread_escape_posix.c:11:9 in add_to_buffer (thread entries: thread_func)
// 	     ↳ conflicting access: write at ${REPO_ROOT}/tests/fixtures/concurrency/thread-escape/thread_escape_posix.c:13:21 in add_to_buffer (thread entries: thread_func)
// 	     ↳ possible conflict kinds: read/write
// 	     ↳ no common recognized lock protects the conflicting accesses
// 	     ↳ additional conflicting access pairs on this location: 2
// 	related: Conflicting access -> ${REPO_ROOT}/tests/fixtures/concurrency/thread-escape/thread_escape_posix.c:13:21 in add_to_buffer
// 	related: Conflicting site -> ${REPO_ROOT}/tests/fixtures/concurrency/thread-escape/thread_escape_posix.c:12:23 in add_to_buffer
//
// Function: add_to_buffer
// 	severity: ERROR
// 	ruleId: DataRaceGlobal
// 	cwe: CWE-362
// 	symbol: shared_buffer
// 	at line 12, column 37
// 	[!!!Error] unsynchronized concurrent access to global 'shared_buffer'
// 	     ↳ first access: write at ${REPO_ROOT}/tests/fixtures/concurrency/thread-escape/thread_escape_posix.c:12:37 in add_to_buffer (thread entries: thread_func)
// 	     ↳ conflicting access: write at ${REPO_ROOT}/tests/fixtures/concurrency/thread-escape/thread_escape_posix.c:12:37 in add_to_buffer (thread entries: thread_func)
// 	     ↳ possible conflict kinds: write/write
// 	     ↳ no common recognized lock protects the conflicting accesses
// 	related: Conflicting access -> ${REPO_ROOT}/tests/fixtures/concurrency/thread-escape/thread_escape_posix.c:12:37 in add_to_buffer
// EXPECT-HUMAN-DIAGNOSTICS-END
