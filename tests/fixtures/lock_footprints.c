// SPDX-License-Identifier: Apache-2.0
// Threads started on an object that holds their own handle, each locking something in it: the
// proof that the thread leaves its handle alone reads the lock's bytes from the lock's type, and
// refuses wherever that type, the lock's place or its extent is unknown, or where it overlaps the
// handle (#201). Each `*_start` makes one creation; the unit test asks the proof about it. Several
// of these programs are invalid, a lock over the handle's bytes for one: no data race can show
// their refusal, so the proof is asked directly.
#include <pthread.h>
#include <stddef.h>

#if defined(__APPLE__)
// macOS has no spinlocks; glibc's is a plain integer, as declared here.
typedef volatile int pthread_spinlock_t;
int pthread_spin_lock(pthread_spinlock_t* lock);
int pthread_spin_unlock(pthread_spinlock_t* lock);
#endif

int pick;
static int shared;

static void acquire(pthread_mutex_t* lock)
{
    pthread_mutex_lock(lock);
}

static void release(pthread_mutex_t* lock)
{
    pthread_mutex_unlock(lock);
}

// Accepted: a typed lock declared first, at offset zero.
struct First
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static void* first_entry(void* argument)
{
    struct First* held = argument;
    pthread_mutex_lock(&held->lock);
    shared += 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

void first_start(struct First* task)
{
    pthread_create(&task->thread, NULL, first_entry, task);
}

// Accepted: a typed lock past the handle, beside a value written as data.
struct Valued
{
    pthread_t thread;
    pthread_mutex_t lock;
    int value;
};

static void* valued_entry(void* argument)
{
    struct Valued* held = argument;
    pthread_mutex_lock(&held->lock);
    held->value = 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

void valued_start(struct Valued* task)
{
    pthread_create(&task->thread, NULL, valued_entry, task);
}

// Accepted: a helper locking whatever lock it is handed, given the typed lock.
struct Handed
{
    pthread_t thread;
    pthread_mutex_t lock;
};

static void* handed_entry(void* argument)
{
    struct Handed* held = argument;
    acquire(&held->lock);
    shared += 1;
    release(&held->lock);
    return NULL;
}

void handed_start(struct Handed* task)
{
    pthread_create(&task->thread, NULL, handed_entry, task);
}

// Refused: the lock and the handle share their bytes. Using both is invalid.
struct Overlapping
{
    union
    {
        pthread_mutex_t lock;
        pthread_t thread;
    } u;
};

static void* overlapping_entry(void* argument)
{
    struct Overlapping* held = argument;
    pthread_mutex_lock(&held->u.lock);
    shared += 1;
    pthread_mutex_unlock(&held->u.lock);
    return NULL;
}

void overlapping_start(struct Overlapping* task)
{
    pthread_create(&task->u.thread, NULL, overlapping_entry, task);
}

// Refused: the same lock taken through a helper handed the lock, or the whole object.
static void* overlapping_helper_entry(void* argument)
{
    struct Overlapping* held = argument;
    acquire(&held->u.lock);
    shared += 1;
    release(&held->u.lock);
    return NULL;
}

void overlapping_helper_start(struct Overlapping* task)
{
    pthread_create(&task->u.thread, NULL, overlapping_helper_entry, task);
}

static void lock_overlapping(struct Overlapping* locked)
{
    pthread_mutex_lock(&locked->u.lock);
}

static void* overlapping_object_helper_entry(void* argument)
{
    lock_overlapping(argument);
    shared += 1;
    return NULL;
}

void overlapping_object_helper_start(struct Overlapping* task)
{
    pthread_create(&task->u.thread, NULL, overlapping_object_helper_entry, task);
}

// Refused: the lock reached starts past the typed lock handed, on the handle, or inside the typed
// lock, reaching past its end onto the handle.
struct Beyond
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static void lock_next(pthread_mutex_t* lock)
{
    pthread_mutex_lock(lock + 1);
}

static void* beyond_entry(void* argument)
{
    struct Beyond* held = argument;
    lock_next(&held->lock);
    shared += 1;
    return NULL;
}

void beyond_start(struct Beyond* task)
{
    pthread_create(&task->thread, NULL, beyond_entry, task);
}

static void* inside_entry(void* argument)
{
    struct Beyond* held = argument;
    pthread_mutex_lock((pthread_mutex_t*)((char*)&held->lock + sizeof(int)));
    shared += 1;
    return NULL;
}

void inside_start(struct Beyond* task)
{
    pthread_create(&task->thread, NULL, inside_entry, task);
}

// Refused: the lock is formed in bytes whose type names no lock, too few to hold one; past them
// lies the handle. Invalid too.
struct Untyped
{
    unsigned char bytes[8];
    pthread_t thread;
};

static void* untyped_entry(void* argument)
{
    struct Untyped* held = argument;
    pthread_mutex_lock((pthread_mutex_t*)held->bytes);
    shared += 1;
    pthread_mutex_unlock((pthread_mutex_t*)held->bytes);
    return NULL;
}

void untyped_start(struct Untyped* task)
{
    pthread_create(&task->thread, NULL, untyped_entry, task);
}

// Refused: the entry locks the object it is handed as a whole; nothing types the lock there.
struct Whole
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static void* whole_entry(void* argument)
{
    pthread_mutex_lock(argument);
    shared += 1;
    pthread_mutex_unlock(argument);
    return NULL;
}

void whole_start(struct Whole* task)
{
    pthread_create(&task->thread, NULL, whole_entry, task);
}

// Refused: the lock is reached through a pointer read out of the object.
struct Loaded
{
    pthread_mutex_t* lock;
    pthread_t thread;
};

static void* loaded_entry(void* argument)
{
    struct Loaded* held = argument;
    pthread_mutex_lock(held->lock);
    shared += 1;
    pthread_mutex_unlock(held->lock);
    return NULL;
}

void loaded_start(struct Loaded* task)
{
    pthread_create(&task->thread, NULL, loaded_entry, task);
}

// Refused: the lock's place depends on a value the entry reads, directly or through a helper.
struct Indexed
{
    pthread_mutex_t locks[2];
    pthread_t thread;
};

static void* indexed_entry(void* argument)
{
    struct Indexed* held = argument;
    pthread_mutex_lock(&held->locks[pick]);
    shared += 1;
    pthread_mutex_unlock(&held->locks[pick]);
    return NULL;
}

void indexed_start(struct Indexed* task)
{
    pthread_create(&task->thread, NULL, indexed_entry, task);
}

static void* indexed_helper_entry(void* argument)
{
    struct Indexed* held = argument;
    acquire(&held->locks[pick]);
    shared += 1;
    release(&held->locks[pick]);
    return NULL;
}

void indexed_helper_start(struct Indexed* task)
{
    pthread_create(&task->thread, NULL, indexed_helper_entry, task);
}

// Refused: a helper locking the guard it is handed, given one at a place read at run time.
struct Guard
{
    pthread_mutex_t lock;
};

struct Guards
{
    struct Guard guards[2];
    pthread_t thread;
};

static void enter(struct Guard* guard)
{
    pthread_mutex_lock(&guard->lock);
}

static void* guards_entry(void* argument)
{
    struct Guards* held = argument;
    enter(&held->guards[pick]);
    shared += 1;
    return NULL;
}

void guards_start(struct Guards* task)
{
    pthread_create(&task->thread, NULL, guards_entry, task);
}

// Refused: a spinlock is a plain integer, whose type names no lock.
struct Spinning
{
    pthread_spinlock_t spin;
    pthread_t thread;
};

static void* spinning_entry(void* argument)
{
    struct Spinning* held = argument;
    pthread_spin_lock(&held->spin);
    shared += 1;
    pthread_spin_unlock(&held->spin);
    return NULL;
}

void spinning_start(struct Spinning* task)
{
    pthread_create(&task->thread, NULL, spinning_entry, task);
}

// Refused: a function the classifier does not recognize is handed the typed lock; this unit does
// not define it.
struct Unrecognized
{
    pthread_mutex_t lock;
    pthread_t thread;
};

void lock_elsewhere(pthread_mutex_t* lock);

static void* unrecognized_entry(void* argument)
{
    struct Unrecognized* held = argument;
    lock_elsewhere(&held->lock);
    shared += 1;
    return NULL;
}

void unrecognized_start(struct Unrecognized* task)
{
    pthread_create(&task->thread, NULL, unrecognized_entry, task);
}
