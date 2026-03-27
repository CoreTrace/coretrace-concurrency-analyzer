// Test 9: C++ - Double-checked locking pattern cassé (avant C++11 proper)
#include <thread>
#include <mutex>
#include <iostream>

class Singleton {
private:
    static Singleton* instance;  // Pointeur partagé
    static std::mutex mtx;
    int data;
    
    Singleton() : data(42) {}
    
public:
    static Singleton* getInstance() {
        if (instance == nullptr) {  // Premier check (sans lock)
            std::lock_guard<std::mutex> lock(mtx);
            if (instance == nullptr) {  // Deuxième check (avec lock)
                instance = new Singleton();  // DATA RACE: publication non atomique
            }
        }
        return instance;
    }
    
    int getData() const { return data; }
};

// Initialisation statique
Singleton* Singleton::instance = nullptr;
std::mutex Singleton::mtx;

void worker(int id) {
    Singleton* s = Singleton::getInstance();
    std::cout << "Thread " << id << " got instance with data: " 
              << s->getData() << std::endl;
}

int main() {
    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    std::thread t3(worker, 3);
    
    t1.join();
    t2.join();
    t3.join();
    
    delete Singleton::instance;
    return 0;
}
