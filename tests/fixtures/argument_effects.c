// SPDX-License-Identifier: Apache-2.0
// Calls handed an object, for what each may do to it (#206): declarations with and without what
// LLVM states about their argument, definitions that only read it, keep it, return it, write
// memory they do not place, or take it past their parameters, and the two ends of a copy.
#include <string.h>

struct pair
{
    long first;
    long second;
};

void touch(__attribute__((noescape)) struct pair* pair);
void keep(struct pair* pair);
__attribute__((pure)) long peek(const struct pair* pair);
__attribute__((pure)) long peek_in_place(__attribute__((noescape)) const struct pair* pair);

static long sum(const struct pair* pair)
{
    return pair->first + pair->second;
}

static void save(struct pair* pair, struct pair** slot)
{
    *slot = pair;
}

static struct pair* same(struct pair* pair)
{
    return pair;
}

static long tallied;

static void tally(const struct pair* pair)
{
    (void)pair;
    ++tallied;
}

static void forward(int count, ...)
{
    (void)count;
}

long exercise(struct pair** slot)
{
    struct pair pair;
    struct pair copy;
    pair.first = 1;
    pair.second = 2;
    touch(&pair);
    keep(&pair);
    long total = peek(&pair) + peek_in_place(&pair) + sum(&pair);
    save(&pair, slot);
    same(&pair)->second = 3;
    tally(&pair);
    forward(1, &pair);
    memcpy(&copy, &pair, sizeof pair);
    return total + copy.first;
}
