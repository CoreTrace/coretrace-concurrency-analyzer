// Test 10: C++ - Mélange d'opérations atomiques et non-atomiques
#include <atomic>
#include <thread>
#include <iostream>

struct SharedState {
    std::atomic<int> counter{0};  // Atomique
    int total{0};                  // NON atomique - DATA RACE!
};

SharedState state;

void worker() {
    for (int i = 0; i < 1000; i++) {
        state.counter++;  // OK: atomique
        state.total++;    // DATA RACE: non atomique
    }
}

int main() {
    std::thread t1(worker);
    std::thread t2(worker);
    std::thread t3(worker);
    
    t1.join();
    t2.join();
    t3.join();
    
    std::cout << "Atomic counter: " << state.counter << std::endl;
    std::cout << "Non-atomic total: " << state.total 
              << " (expected: 3000)" << std::endl;
    
    return 0;
}
