// SPDX-License-Identifier: Apache-2.0
// Test: Condition variable - prédicat périmé avec broadcast et plusieurs consommateurs
//
// Distinct de condition_variable_spurious.c (réveil spurieux, un seul consommateur) :
// ici le bug est la SURCONSOMMATION avec plusieurs consommateurs et broadcast.
// Le producteur diffuse (broadcast) ; les deux consommateurs se réveillent.
// Le premier traite la donnée et pose data_ready = 0. Le second, réveillé par
// le même broadcast, reprend sans revérifier le prédicat (if au lieu de while) :
// il lit une donnée déjà consommée (data_value non réinitialisé) et repose
// data_ready = 0 une seconde fois — comportement non déterministe.
// Correction : utiliser while (!data_ready) pour revérifier après chaque réveil.
#include <pthread.h>
#include <stdio.h>

pthread_mutex_t mtx   = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;
int data_ready = 0;
int data_value = 0;

void* producer(void* arg) {
    pthread_mutex_lock(&mtx);
    data_value = 42;
    data_ready = 1;
    pthread_cond_broadcast(&cond);  // Réveille TOUS les consommateurs simultanément
    pthread_mutex_unlock(&mtx);
    return NULL;
}

void* consumer(void* arg) {
    pthread_mutex_lock(&mtx);
    if (!data_ready) {
        // CONDITION VARIABLE: 'if' au lieu de 'while' — le prédicat n'est pas
        // revérifié après le réveil. Le second consommateur réveillé par le broadcast
        // peut voir data_ready = 0 (premier consommateur l'a remis à 0) et quand même
        // lire data_value, qui n'est plus valide pour lui.
        pthread_cond_wait(&cond, &mtx);
    }
    printf("Consumer lit: %d (data_ready=%d)\n", data_value, data_ready);
    data_ready = 0;  // Le second consommateur écrase ce que le premier a déjà remis à 0
    pthread_mutex_unlock(&mtx);
    return NULL;
}

int main() {
    pthread_t prod, cons1, cons2;
    pthread_create(&cons1, NULL, consumer, NULL);
    pthread_create(&cons2, NULL, consumer, NULL);
    pthread_create(&prod, NULL, producer, NULL);
    pthread_join(prod, NULL);
    pthread_join(cons1, NULL);
    pthread_join(cons2, NULL);
    return 0;
}
