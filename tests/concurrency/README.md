# Tests de bugs de concurrence

Cette collection contient des exemples de bugs de concurrence en C et C++ pour tester l'analyseur CoreTrace.

## Structure des dossiers

```
tests/concurrency/
├── data-race/          # Data races (lectures/écritures concurrentes non protégées)
├── deadlock/           # Deadlocks (attentes circulaires de locks)
├── memory-barrier/     # Problèmes de barrières mémoire (réordering CPU)
├── condition-variable/ # Mauvaise utilisation des condition variables
├── thread-escape/      # Threads échappant au contrôle (fork, main thread)
└── missing-join/       # Threads non joints (ressources perdues, synchronisation manquante)
```

## Catégories de tests

### data-race/
- **data_race_basic.c**: Écriture concurrente sur un compteur partagé
- **data_race_mixed_access.c**: Lectures et écritures mélangées sans synchronisation
- **race_condition_check_then_use.c**: Pattern TOCTOU (check-then-use)
- **cpp_data_race_class.cpp**: Data race dans une classe C++ non thread-safe
- **cpp_race_std_async.cpp**: Data race avec std::async et shared state
- **cpp_atomic_vs_non_atomic.cpp**: Mélange dangereux d'opérations atomiques et non-atomiques
- **cpp_move_semantics_race.cpp**: Data race avec move semantics et unique_ptr
- **cpp_double_checked_locking.cpp**: Double-checked locking pattern cassé

### deadlock/
- **deadlock_basic.c**: Acquisition de deux locks dans ordre inverse par deux threads
- **lock_order_violation.c**: Violation cyclique d'ordre de locks (3+ mutex)
- **recursive_deadlock.c**: Thread s'attend lui-même (lock non-récursif)

### memory-barrier/
- **missing_memory_barrier.c**: Réordering CPU/compiler sans barrière mémoire appropriée

### condition-variable/
- **condition_variable_spurious.c**: Spurious wakeup non géré (if au lieu de while)

### thread-escape/
- **thread_escape_posix.c**: Fonction appelée depuis un thread ET depuis main sans synchronisation
- **fork_thread_race.c**: Race condition après fork() avec threads existants

### missing-join/
- **missing_join_basic.c**: Thread unique jamais joint, mémoire potentiellement perdue
- **missing_join_multiple.c**: Plusieurs threads créés, un seul est joint
- **missing_join_detach_mix.c**: Mélange incorrect de detach et threads non joints
- **cpp_missing_join.cpp**: Threads std::thread non joints (risque de std::terminate())

## Compilation

### Pour les fichiers C:
```bash
gcc -pthread -o test tests/concurrency/data-race/data_race_basic.c
```

### Pour les fichiers C++:
```bash
g++ -std=c++17 -pthread -o test tests/concurrency/data-race/cpp_data_race_class.cpp
```

### Avec instrumentation CoreTrace:
```bash
./build-llvm20/coretrace_concurrency_analyzer tests/concurrency/data-race/data_race_basic.c --ir-format=ll
./build-llvm20/coretrace_concurrency_analyzer tests/concurrency/deadlock/deadlock_basic.c --ir-format=bc
```

## Exécution attendue

Ces tests sont conçus pour être **détectés statiquement** par l'analyseur CoreTrace. Certains bugs peuvent ne pas se manifester à l'exécution car ils dépendent du scheduling des threads.

| Catégorie | Détection statique | Reproduction dynamique |
|-----------|-------------------|------------------------|
| data-race | ✅ | ⚠️ (non déterministe) |
| deadlock | ✅ | ⚠️ (dépend du timing) |
| memory-barrier | ✅ | ❌ (rarement observable) |
| condition-variable | ✅ | ⚠️ (spurious wakeups rares) |
| thread-escape | ✅ | ⚠️ (dépend du contexte) |
| missing-join | ✅ | ✅ (fuites mémoire visibles) |

## Ajout de nouveaux tests

Pour ajouter un nouveau test :
1. Créer le fichier dans le sous-dossier approprié
2. Ajouter un commentaire expliquant le bug au début du fichier
3. Mettre à jour ce README
4. Vérifier que l'analyseur détecte le bug
