# Tests de bugs de concurrence

Cette collection contient des exemples de bugs de concurrence en C et C++ pour
tester l'analyseur CoreTrace, ainsi que des variantes correctes qui ne doivent
rien signaler.

## Ce que chaque fixture vérifie

Chaque fichier `.c` ou `.cpp` de ce dossier a une ligne dans la table
`fixtureExpectations()` de `tests/unit/test_concurrency_analysis.cpp` : son
intention (`intent`) et le nombre de diagnostics attendus pour chaque règle.
`testFixtureExpectationTable` vérifie ces nombres, et
`testEveryConcurrencyFixtureIsCovered` échoue dès qu'un fichier n'a pas de
ligne. Cette table est la liste de référence ; ce README ne la recopie pas. La
même table couvre le dossier voisin `tests/fixtures/concurrency-cxx20/`.

Un suffixe `_no_fp`, `_no_diagnostic`, `_no_missing_join` ou `_no_race` marque
une variante correcte que la règle visée ne doit pas signaler. Une ligne sans
diagnostic attendu peut aussi documenter une limite connue de l'analyseur : son
`intent` le dit alors, avec le numéro de l'issue.

## Structure des dossiers

```
tests/fixtures/concurrency/
├── atomic-ordering/    # Publication par atomiques, ordres mémoire trop faibles
├── condition-variable/ # Mauvaise utilisation des condition variables
├── data-race/          # Lectures/écritures concurrentes non protégées
├── deadlock/           # Attentes circulaires de locks
├── memory-barrier/     # Réordonnancement sans barrière mémoire
├── missing-join/       # Threads jamais joints
├── once-init/          # Initialisation unique (DCLP, statiques locales, pthread_once)
├── process/            # fork() après création de threads, processus fils jamais attendus
├── signal/             # Handlers de signal : cas écrits pour la règle unsafe-signal-handler
├── signal-handler/     # Handlers de signal : exemples importés du corpus initial
├── thread-escape/      # États qui échappent à leur thread (pile du créateur, fork, main)
├── thread-local/       # Variables thread-locales : destructeurs, adresses après la fin du thread
└── use-after-free/     # Mémoire libérée pendant qu'un thread l'utilise encore
```

## Analyser une fixture

```bash
./build-llvm20/coretrace_concurrency_analyzer tests/fixtures/concurrency/data-race/data_race_basic.c --analyze
./build-llvm20/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=deadlock-lock-order --format=json
```

`build-llvm20` est le dossier de build décrit dans le README principal. Sans
`--analyze`, l'analyseur compile seulement la fixture en IR.

## Compiler et exécuter une fixture

```bash
gcc -pthread -o test tests/fixtures/concurrency/data-race/data_race_basic.c
g++ -std=c++20 -pthread -o test tests/fixtures/concurrency/data-race/cpp_data_race_class.cpp
```

Ces fixtures sont faites pour l'analyse statique : beaucoup de ces bugs
dépendent de l'ordonnancement des threads ou du processeur et peuvent ne pas se
manifester à l'exécution.

## Ajouter une fixture

1. Créer le fichier dans le sous-dossier approprié, avec
   `// SPDX-License-Identifier: Apache-2.0` en première ligne
   (`scripts/check-license-compliance.sh` le vérifie).
2. Expliquer en tête de fichier le bug, ou pourquoi la variante est correcte.
3. Ajouter sa ligne dans `fixtureExpectations()` : son intention et le nombre
   de diagnostics attendus pour chaque règle.
4. Lancer `ctest --test-dir build-llvm20 -R coretrace_concurrency_analysis_tests --output-on-failure`.
