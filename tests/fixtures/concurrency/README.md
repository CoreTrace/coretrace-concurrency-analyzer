# Tests de bugs de concurrence

Cette collection contient des exemples de bugs de concurrence en C et C++ pour tester l'analyseur CoreTrace.

## Structure des dossiers

```
tests/fixtures/concurrency/
├── data-race/          # Data races (lectures/écritures concurrentes non protégées)
├── deadlock/           # Deadlocks (attentes circulaires de locks)
├── memory-barrier/     # Problèmes de barrières mémoire (réordering CPU)
├── condition-variable/ # Mauvaise utilisation des condition variables
├── thread-escape/      # Threads échappant au contrôle (fork, main thread)
├── missing-join/       # Threads non joints (ressources perdues, synchronisation manquante)
├── once-init/          # Initialisation unique incorrecte (DCLP, drapeaux non atomiques)
├── atomic-ordering/    # Mauvais ordres mémoire atomiques (relaxed, seqlock, store-load)
├── use-after-free/     # Accès à mémoire libérée depuis un thread concurrent
├── signal-handler/     # Mauvaise utilisation des handlers de signal (async-signal-safety)
└── thread-local/       # Mauvaise utilisation des variables thread-locales (TLS)
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
- **cpp_shared_ptr_race.cpp**: Data race sur un `std::shared_ptr` partagé (copy + reset sans synchronisation — UB même si le ref count est atomique)
- **cpp_static_local_init_side_effect.cpp**: Init statique locale thread-safe (C++11) mais avec effets de bord non protégés sur un registre global
- **cpp_lambda_capture_race.cpp**: Data race via capture par référence dans un lambda passé à plusieurs `std::thread`
- **cpp_iterator_invalidation_race.cpp**: Insertion dans `std::vector` pendant itération — invalidation d'itérateur + data race
- **c_bitfield_race.c**: Accès concurrent à des bitfields distincts partageant la même unité de stockage — le read-modify-write de l'unité entière crée une data race

### deadlock/
- **deadlock_basic.c**: Acquisition de deux locks dans ordre inverse par deux threads
- **lock_order_violation.c**: Violation cyclique d'ordre de locks (3+ mutex)
- **recursive_deadlock.c**: Thread s'attend lui-même (lock non-récursif)
- **cpp_scoped_lock_order.cpp**: ABBA deadlock avec `std::lock_guard` dans ordre inverse (sans `std::lock` ni `std::scoped_lock`)
- **self_deadlock_recursive.c**: Thread verrouille un `pthread_mutex_t` non récursif puis appelle une fonction qui verrouille le même mutex
- **cpp_future_mutex_deadlock.cpp**: `std::future::get()` appelé sous un mutex, pendant que la tâche async attend ce même mutex pour produire sa valeur

### memory-barrier/
- **missing_memory_barrier.c**: Réordering CPU/compiler sans barrière mémoire appropriée

### condition-variable/
- **condition_variable_spurious.c**: Spurious wakeup non géré (if au lieu de while)
- **condition_variable_destructor_race.cpp**: Destruction de `std::condition_variable` pendant qu'un thread est bloqué dans `wait()` — comportement indéfini

### thread-escape/
- **thread_escape_posix.c**: Fonction appelée depuis un thread ET depuis main sans synchronisation
- **fork_thread_race.c**: Race condition après fork() avec threads existants

### missing-join/
- **missing_join_basic.c**: Thread unique jamais joint, mémoire potentiellement perdue
- **missing_join_multiple.c**: Plusieurs threads créés, un seul est joint
- **missing_join_detach_mix.c**: Mélange incorrect de detach et threads non joints
- **cpp_missing_join.cpp**: Threads std::thread non joints (risque de std::terminate())

### once-init/
- **once_init_double_checked_locking.c**: DCLP avec `volatile` incorrect comme tentative de correction — aucune barrière mémoire réelle
- **once_init_manual_flag_race.c**: Drapeau `int initialized` sans mutex — plusieurs threads peuvent initialiser simultanément
- **cpp_once_init_broken_dclp.cpp**: DCLP C++ sur `Singleton*` brut sans `std::atomic` — publication non sûre (style pré-C++11)

### atomic-ordering/
- **store_load_reorder.cpp**: Test de Dekker avec `memory_order_relaxed` — réordering store-load permet aux deux threads d'entrer simultanément en section critique (violant l'exclusion mutuelle)
- **seqlock_missing_fence.cpp**: Seqlock dont le lecteur utilise `memory_order_relaxed` au lieu d'`acquire` — les lectures de données peuvent être réordonnées avant/après le compteur de séquence, causant des lectures déchirées

### use-after-free/
- **use_after_free_callback.c**: Struct de travail (avec pointeur de callback) libéré immédiatement après soumission au thread worker qui le déréférence encore
- **cpp_detached_thread_dangling_ref.cpp**: Thread détaché capturant une variable locale par référence — la variable est détruite au retour de la fonction englobante pendant que le thread tourne

### signal-handler/
- **signal_mutex_deadlock.c**: Handler appelle `pthread_mutex_lock()` (non async-signal-safe) — deadlock si le signal interrompt le thread pendant qu'il tient le même mutex
- **signal_reentrant_function.c**: Handler appelle `strtok()` (non réentrant) pendant qu'un appel `strtok()` est en cours dans main — le contexte interne est écrasé
- **signal_thread_mask.c**: Workers sans `pthread_sigmask` — un signal destiné à main peut être livré à un worker qui modifie des données "main-only" sans synchronisation

### thread-local/
- **cpp_tls_destructor_race.cpp**: Destructeur d'objet `thread_local` décrémentant un compteur global non protégé — plusieurs threads terminent simultanément, leurs destructeurs TLS font des data races
- **c_tls_dangling_ptr.c**: Adresse d'une variable `_Thread_local` publiée dans un pointeur global, déréférencée après terminaison du thread — use-after-free sur la TLS libérée

## Compilation

### Pour les fichiers C:
```bash
gcc -pthread -o test tests/fixtures/concurrency/data-race/data_race_basic.c
```

### Pour les fichiers C++:
```bash
g++ -std=c++17 -pthread -o test tests/fixtures/concurrency/data-race/cpp_data_race_class.cpp
```

### Avec instrumentation CoreTrace:
```bash
./build-llvm20/coretrace_concurrency_analyzer tests/fixtures/concurrency/data-race/data_race_basic.c --ir-format=ll
./build-llvm20/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --ir-format=bc
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
| once-init | ✅ | ⚠️ (non déterministe) |
| atomic-ordering | ✅ | ❌ (dépend de l'architecture CPU) |
| use-after-free | ✅ | ⚠️ (non déterministe) |
| signal-handler | ✅ | ⚠️ (dépend du timing des signaux) |
| thread-local | ✅ | ⚠️ (dépend du scheduling à la terminaison) |

## Ajout de nouveaux tests

Pour ajouter un nouveau test :
1. Créer le fichier dans le sous-dossier approprié
2. Ajouter un commentaire expliquant le bug au début du fichier
3. Mettre à jour ce README
4. Vérifier que l'analyseur détecte le bug
