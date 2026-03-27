// Test 8: C++ - Data race avec std::async et shared state
#include <future>
#include <iostream>
#include <vector>

std::vector<int> shared_vector;  // Non protégé

void producer(int id) {
    for (int i = 0; i < 100; i++) {
        shared_vector.push_back(id * 1000 + i);  // DATA RACE: modification concurrente
    }
}

int consumer() {
    int sum = 0;
    for (size_t i = 0; i < shared_vector.size(); i++) {  // DATA RACE: lecture pendant écriture
        sum += shared_vector[i];
    }
    return sum;
}

int main() {
    std::vector<std::future<void>> futures;
    
    // Lancement de plusieurs producers
    for (int i = 0; i < 3; i++) {
        futures.push_back(std::async(std::launch::async, producer, i));
    }
    
    // Consumer pendant que les producers travaillent
    auto consumer_future = std::async(std::launch::async, consumer);
    
    for (auto& f : futures) {
        f.wait();
    }
    
    std::cout << "Vector size: " << shared_vector.size() << std::endl;
    std::cout << "Sum: " << consumer_future.get() << std::endl;
    
    return 0;
}
