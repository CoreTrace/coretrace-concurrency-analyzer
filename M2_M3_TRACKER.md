# M2 / M3 Tracker

Ce document liste uniquement ce qu'il reste a faire pour completer `M2` et `M3`.
Chaque point commence par une action, pour rester directement executable.

## Regle De Mise A Jour

- `Status`: utiliser `TODO`, `IN_PROGRESS` ou `DONE`.
- `Tests`: decrire les tests ajoutes ou executes pour valider la tache.
- Quand une tache est terminee, remplacer son statut par `DONE` et renseigner `Tests`.
- Si une tache necessite une modification de tests du depot, demander une autorisation explicite avant de toucher aux fichiers de tests.

## M2 - Missing Join Et Deadlock Basique

### M2.1 - Etendre Le Modele De Diagnostic

#### Ajouter les nouvelles regles publiques pour `MissingJoin` et `DeadlockLockOrder`
- Status: `DONE`
- Pourquoi: l'API publique ne decrit aujourd'hui que `CompilerDiagnostic` et `DataRaceGlobal`; il faut des identifiants stables avant d'ajouter les nouveaux checkers.
- Cibles probables:
  `include/coretrace_concurrency_analysis.hpp`
  `src/internal/diagnostics/diagnostic_catalog.hpp`
  `src/internal/diagnostics/diagnostic_catalog.cpp`
- Tests: `cmake --build build -j4`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_error_tests`, `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Ajouter les metadonnees de rendu pour les nouvelles regles
- Status: `DONE`
- Pourquoi: les sorties `human`, `json` et `sarif` doivent rester coherentes sans logique speciale par format.
- Cibles probables:
  `src/internal/diagnostics/diagnostic_catalog.cpp`
  `src/internal/reporting/report_renderer.cpp`
- Tests: `cmake --build build -j4`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_error_tests`, `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

### M2.2 - Etendre Les Facts Pour Les Handles Et Les Ordres De Lock

#### Introduire des facts de cycle de vie de thread pour tracer creation, join et detach
- Status: `DONE`
- Pourquoi: `MissingJoinDetector` doit s'appuyer sur des faits reutilisables plutot que reparcourir le module avec une logique dupliquee.
- Cibles probables:
  `src/internal/analysis/facts.hpp`
  `src/internal/analysis/tu_facts_builder.cpp`
  `src/internal/analysis/thread_spawn_detector.cpp`
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_basic.c --analyze --rules=missing-join --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_multiple.c --analyze --rules=missing-join --format=human` et `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_detach_mix.c --analyze --rules=missing-join --format=human`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Introduire des facts de sequence d'acquisition de locks par fonction
- Status: `DONE`
- Pourquoi: `LockOrderAnalyzer` doit comparer des ordres de locks normalises, pas reparser directement l'IR pendant le croisement final.
- Cibles probables:
  `src/internal/analysis/facts.hpp`
  `src/internal/analysis/lock_scope_tracker.cpp`
  `src/internal/analysis/tu_facts_builder.cpp`
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=deadlock-lock-order --format=human` et `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=all --format=human`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

### M2.3 - Implementer Les Checkers

#### Implementer `MissingJoinDetector` sur les facts de creation/join/detach pour les handles pthread
- Status: `DONE`
- Pourquoi: separer ce checker de `DataRaceChecker` garde l'architecture extensible pour les regles futures, et le premier perimetre utile est le cycle de vie `pthread` avec normalisation de handles et de symboles ABI.
- Cibles probables:
  `src/internal/analysis/missing_join_detector.hpp`
  `src/internal/analysis/missing_join_detector.cpp`
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_basic.c --analyze --rules=missing-join --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_multiple.c --analyze --rules=missing-join --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_detach_mix.c --analyze --rules=missing-join --format=human` et `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=all --format=human` pour verifier qu'un `pthread_join` reconnu n'emette plus de faux positif; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Etendre `MissingJoinDetector` au cycle de vie `std::thread` sans faux positifs sur move semantics
- Status: `DONE`
- Pourquoi: les facts existent deja, mais le groupement des objets `std::thread` deplaces ou stockes en conteneur doit rester precis avant activation large.
- Cibles probables:
  `src/internal/analysis/missing_join_detector.hpp`
  `src/internal/analysis/missing_join_detector.cpp`
  `src/internal/analysis/ir_utils.hpp`
  `src/internal/analysis/ir_utils.cpp`
  `src/internal/analysis/concurrency_symbol_classifier.cpp`
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/data-race/cpp_thread_local_class.cpp --analyze --rules=missing-join --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/cpp_missing_join.cpp --analyze --rules=missing-join --format=human` et `./build/coretrace_concurrency_analyzer /tmp/coretrace_std_missing_join.cpp --analyze --rules=missing-join --format=human`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_error_tests`, `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Implementer `LockOrderAnalyzer` pour detecter les inversions simples d'ordre de locks
- Status: `DONE`
- Pourquoi: l'analyse d'ordre de locks est conceptuellement differente de la data race et doit rester dans un composant autonome.
- Cibles probables:
  `src/internal/analysis/lock_order_analyzer.hpp`
  `src/internal/analysis/lock_order_analyzer.cpp`
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=deadlock-lock-order --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=all --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/lock_order_violation.c --analyze --rules=deadlock-lock-order --format=human` et `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/recursive_deadlock.c --analyze --rules=deadlock-lock-order --format=human`; le perimetre actuellement livre couvre les inversions simples et la reacquisition interprocedurale simple via appel direct, mais ne couvre pas encore les cycles 3+; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

### M2.4 - Integrer Les Nouveaux Checkers Dans Le Pipeline

#### Composer `SingleTUConcurrencyAnalyzer` avec plusieurs checkers au lieu d'un seul checker inline
- Status: `DONE`
- Pourquoi: une composition explicite de checkers evite de transformer `DataRaceChecker` en classe monolithique.
- Cibles probables:
  `src/coretrace_concurrency_analysis.cpp`
  `include/coretrace_concurrency_analysis.hpp`
- Tests: `cmake --build build -j4`; validation manuelle du chemin historique avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/data-race/data_race_basic.c --analyze --format=human`; validation manuelle des nouveaux chemins avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_basic.c --analyze --rules=missing-join --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=deadlock-lock-order --format=human` et `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=all --format=human`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Introduire une selection explicite des regles dans l'API et le CLI pour activer les nouveaux checkers sans casser le comportement historique
- Status: `DONE`
- Pourquoi: activer `MissingJoin` et `DeadlockLockOrder` par defaut cassait les sorties historiques des fixtures `data-race`; une selection explicite garde la retro-compatibilite tout en laissant `M2` progresser proprement.
- Cibles probables:
  `include/coretrace_concurrency_analysis.hpp`
  `src/coretrace_concurrency_analysis.cpp`
  `main.cpp`
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/data-race/data_race_basic.c --analyze --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=all --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/hello.c --rules=all` et `./build/coretrace_concurrency_analyzer --help`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Etendre le resume de diagnostic pour refleter `MissingJoin` et `DeadlockLockOrder`
- Status: `DONE`
- Pourquoi: le rapport final doit agreger toutes les regles sans traitement special cote CLI.
- Cibles probables:
  `src/internal/analysis/data_race_checker.cpp`
  ou nouveau composant commun d'agregation si extrait
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/missing-join/missing_join_basic.c --analyze --rules=missing-join --format=human`, `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=deadlock-lock-order --format=human` et `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/deadlock_basic.c --analyze --rules=all --format=human`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

### M2.5 - Valider La Feature

#### Ajouter les tests `missing-join` sur fixtures existantes apres autorisation explicite
- Status: `DONE`
- Pourquoi: les fixtures existent deja, mais la modification des fichiers de tests du depot requiert une autorisation explicite.
- Cibles probables:
  `tests/unit/test_concurrency_analysis.cpp`
  `tests/integration/cli/test_cli_cpp.cpp`
  `tests/integration/cli/test_human_output_golden.py`
- Tests: ajout de cas `MissingJoin` dans `tests/unit/test_concurrency_analysis.cpp` pour `missing_join_basic.c`, `missing_join_detach_mix.c`, `cpp_std_thread_missing_join.cpp` et `cpp_missing_join.cpp`; ajout de cas CLI dans `tests/integration/cli/test_cli_cpp.cpp` pour le defaut `--analyze`, le filtrage `--rules=missing-join`, le filtrage multi-regles et la validation `--rules` sans `--analyze`; ajout de la fixture `tests/fixtures/concurrency/missing-join/cpp_std_thread_missing_join.cpp`; execution de `cmake --build build -j4`, `./build/coretrace_concurrency_analysis_tests`, `./build/coretrace_concurrency_cli_cpp_tests` et `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_error_tests`, `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Ajouter les tests `deadlock` sur fixtures existantes apres autorisation explicite
- Status: `DONE`
- Pourquoi: il faut verrouiller le comportement attendu avant de poursuivre vers `M3`.
- Cibles probables:
  `tests/unit/test_concurrency_analysis.cpp`
  `tests/integration/cli/test_cli_cpp.cpp`
  `tests/integration/cli/test_human_output_golden.py`
- Tests: ajout de cas `DeadlockLockOrder` dans `tests/unit/test_concurrency_analysis.cpp` pour `deadlock_basic.c` et `recursive_deadlock.c`; ajout de cas CLI dans `tests/integration/cli/test_cli_cpp.cpp` pour le defaut `--analyze` sur `deadlock_basic.c` et le filtre `--rules=deadlock-lock-order` sur `recursive_deadlock.c`; execution de `cmake --build build -j4`, `./build/coretrace_concurrency_analysis_tests`, `./build/coretrace_concurrency_cli_cpp_tests` et `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_error_tests`, `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

## M3 - Alias Analysis Et Propagation Inter-Procedurale

### M3.1 - Factoriser L'Infrastructure Inter-Procedurale

#### Extraire une infrastructure partagee de bindings d'appels directs
- Status: `DONE`
- Pourquoi: `tu_facts_builder` et `thread_spawn_detector` embarquent deja des logiques proches; les factoriser reduit la duplication avant d'ajouter encore plus de propagation.
- Cibles probables:
  `src/internal/analysis/thread_spawn_detector.cpp`
  `src/internal/analysis/tu_facts_builder.cpp`
  `src/internal/analysis/interprocedural_bindings.hpp`
  `src/internal/analysis/interprocedural_bindings.cpp`
- Tests: `cmake --build build -j4`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

### M3.2 - Integrer L'Alias Analysis LLVM

#### Introduire un fournisseur d'analyses LLVM pour exposer `AAResults`
- Status: `TODO`
- Pourquoi: `M3` doit utiliser l'infrastructure LLVM standard plutot qu'une heuristique locale figee sur quelques patterns.
- Cibles probables:
  `src/internal/analysis/llvm_function_analysis_provider.hpp`
  `src/internal/analysis/llvm_function_analysis_provider.cpp`
- Tests: `-`

#### Etendre `AccessFact` avec une provenance de resolution d'alias
- Status: `TODO`
- Pourquoi: distinguer `direct`, `must_alias` et `may_alias` est necessaire pour controler la precision et limiter les faux positifs.
- Cibles probables:
  `src/internal/analysis/facts.hpp`
  `include/coretrace_concurrency_analysis.hpp`
- Tests: `-`

#### Ajouter un fallback alias-aware pour resoudre les acces pointeurs vers des globals
- Status: `TODO`
- Pourquoi: la resolution manuelle actuelle couvre surtout les copies simples; `M3` doit attraper les acces indirects plus generiques.
- Cibles probables:
  `src/internal/analysis/shared_access_collector.cpp`
  `src/internal/analysis/ir_utils.hpp`
  `src/internal/analysis/ir_utils.cpp`
- Tests: `-`

### M3.3 - Materialiser Le Contexte Thread Dans Les Facts

#### Introduire `ThreadContextPropagator` pour pre-calculer l'atteignabilite thread
- Status: `DONE`
- Pourquoi: le contexte thread ne doit plus etre recalcule dans `DataRaceChecker`; il doit devenir un fait reutilisable par tous les checkers.
- Cibles probables:
  `src/internal/analysis/thread_context_propagator.hpp`
  `src/internal/analysis/thread_context_propagator.cpp`
  `src/internal/analysis/facts.hpp`
  `src/internal/analysis/tu_facts_builder.cpp`
- Tests: `cmake --build build -j4`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Remplacer le calcul local de reachability dans `DataRaceChecker` par les facts propagés
- Status: `DONE`
- Pourquoi: le checker doit consommer des faits stabilises, pas reconstruire sa propre vue du call graph.
- Cibles probables:
  `src/internal/analysis/data_race_checker.cpp`
- Tests: `cmake --build build -j4`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

### M3.4 - Propager L'Etat Des Locks A Travers Les Appels

#### Introduire `LockStatePropagator` pour fusionner locks du caller et locks du callee
- Status: `DONE`
- Pourquoi: aujourd'hui la protection par lock est intraprocedurale; `M3` doit reconnaitre qu'un acces dans `f()` peut etre protege parce que `g()` appelle `f()` sous lock.
- Cibles probables:
  `src/internal/analysis/lock_state_propagator.hpp`
  `src/internal/analysis/lock_state_propagator.cpp`
  `src/internal/analysis/facts.hpp`
  `src/internal/analysis/tu_facts_builder.cpp`
- Tests: `cmake --build build -j4`; validation manuelle initiale avec `./build/coretrace_concurrency_analyzer /tmp/coretrace_callsite_lock_protected.c --analyze --format=human` pour verifier qu'un acces global dans un helper appele sous mutex ne remonte plus comme race; validation manuelle avec `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/deadlock/recursive_deadlock.c --analyze --rules=deadlock-lock-order --format=human` pour verifier la propagation des locks d'entree de fonction; ajout de la fixture `tests/fixtures/concurrency/data-race/data_race_callsite_lock_protected.c`; ajout de cas automatises dans `tests/unit/test_concurrency_analysis.cpp` et `tests/integration/cli/test_cli_cpp.cpp`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

#### Mettre a jour les projections d'acces au callsite pour conserver les locks propagés
- Status: `DONE`
- Pourquoi: la projection d'acces existe deja; il faut maintenant lui faire transporter l'etat de protection interprocedural.
- Cibles probables:
  `src/internal/analysis/tu_facts_builder.cpp`
- Tests: `cmake --build build -j4`; validation manuelle avec `./build/coretrace_concurrency_analyzer /tmp/coretrace_callsite_lock_protected.c --analyze --format=human` et `./build/coretrace_concurrency_analyzer tests/fixtures/concurrency/data-race/data_race_basic.c --analyze --format=human`; `ctest --test-dir build --output-on-failure` avec passages de `coretrace_concurrency_analysis_tests` et `coretrace_concurrency_cli_cpp_tests`; echec observe, non lie a cette tache, sur `coretrace_concurrency_arch_tests` avec `BC args should strip stale attached -o variants before appending final pair`

### M3.5 - Stabiliser Le Rapport Et La Precision

#### Exposer la provenance d'alias et la confiance dans les diagnostics si necessaire
- Status: `TODO`
- Pourquoi: les cas `may_alias` doivent pouvoir etre compris et debuggues sans rendre le rapport opaque.
- Cibles probables:
  `include/coretrace_concurrency_analysis.hpp`
  `src/internal/analysis/data_race_checker.cpp`
  `src/internal/reporting/report_renderer.cpp`
- Tests: `-`

#### Mesurer la precision sur les fixtures existantes apres autorisation explicite pour les tests
- Status: `TODO`
- Pourquoi: `M3` n'est complete que si elle reduit effectivement les faux positifs et faux negatifs sur le corpus de fixtures.
- Cibles probables:
  `tests/unit/test_concurrency_analysis.cpp`
  `tests/integration/cli/test_cli_cpp.cpp`
  `tests/integration/cli/test_human_output_golden.py`
- Tests: `autorisation utilisateur requise`
