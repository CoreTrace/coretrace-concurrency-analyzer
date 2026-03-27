// Test 15: C++ - Data race avec move semantics et threads
#include <thread>
#include <vector>
#include <memory>
#include <iostream>

class Resource {
public:
    int value = 0;
};

std::unique_ptr<Resource> shared_resource;  // Non protégé

void producer() {
    auto resource = std::make_unique<Resource>();
    resource->value = 42;
    shared_resource = std::move(resource);  // DATA RACE: écriture du unique_ptr
}

void consumer() {
    if (shared_resource) {  // DATA RACE: lecture
        std::cout << "Value: " << shared_resource->value << std::endl;
    }
}

int main() {
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 3; i++) {
        threads.emplace_back(producer);
    }
    
    for (int i = 0; i < 3; i++) {
        threads.emplace_back(consumer);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    return 0;
}
