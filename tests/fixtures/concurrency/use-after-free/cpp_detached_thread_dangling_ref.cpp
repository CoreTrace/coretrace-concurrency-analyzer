// SPDX-License-Identifier: Apache-2.0
// Test: C++ - Use-after-free via thread détaché capturant une variable locale par référence
//
// Un lambda capture une variable locale par référence et est passé à un std::thread
// qui est immédiatement détaché. La fonction englobante retourne, détruisant la
// variable, pendant que le thread détaché tourne encore et y accède.
// Correction : capturer par valeur, ou joindre le thread avant de retourner.
#include <iostream>
#include <thread>
#include <vector>

void launch_detached() {
    std::vector<int> local_data = {10, 20, 30, 40, 50};

    std::thread t([&local_data]() {
        // Simule un délai — la fonction englobante peut retourner pendant ce temps
        for (volatile int i = 0; i < 2000000; i++);
        // USE-AFTER-FREE: local_data a été détruite au retour de launch_detached()
        for (int v : local_data) {
            std::cout << v << "\n";
        }
    });

    t.detach();
    // local_data est détruite ici : le thread accède à de la mémoire libérée
}

int main() {
    launch_detached();
    // Laisse le thread détaché s'exécuter pendant que local_data est invalide
    for (volatile int i = 0; i < 5000000; i++);
    return 0;
}
