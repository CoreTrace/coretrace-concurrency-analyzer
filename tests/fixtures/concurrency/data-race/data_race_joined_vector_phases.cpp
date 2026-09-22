// SPDX-License-Identifier: Apache-2.0
#include <pthread.h>
#include <vector>
int g;
void* a(void*) { ++g; return nullptr; }
void* b(void*) { ++g; return nullptr; }
void phase(void*(*fn)(void*), unsigned n) { std::vector<pthread_t> h(n); for (unsigned i=0;i<n;++i) pthread_create(&h[i],nullptr,fn,nullptr); for(auto t:h) pthread_join(t,nullptr); }
int main(){phase(a,2);phase(b,2);}
