// SPDX-License-Identifier: Apache-2.0
// Test: C++ - Data race sur une std::map partagée
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <iostream>

std::map<std::string, int> shared_map;  // Non protégée

void insert_entries(int id) {
    for (int i = 0; i < 100; i++) {
        std::string key = "key_" + std::to_string(id) + "_" + std::to_string(i);
        shared_map[key] = i;  // DATA RACE: insertion concurrente invalide sur std::map
    }
}

void read_entries() {
    for (auto& kv : shared_map) {  // DATA RACE: itération pendant modifications
        (void)kv.second;
    }
}

int main() {
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(insert_entries, i);
    }
    threads.emplace_back(read_entries);
    for (auto& t : threads) {
        t.join();
    }
    std::cout << "Map size: " << shared_map.size() << std::endl;
    return 0;
}
