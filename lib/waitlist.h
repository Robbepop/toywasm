#include <stdint.h>

struct atomics_mutex;

struct waiter_list;

struct waiter_list_table {
        struct waiter_list *lists[1];
};

uint32_t atomics_notify(struct waiter_list_table *tab, uint32_t ident,
                        uint32_t count);

void waiter_list_table_init(struct waiter_list_table *tab);

int atomics_wait(struct waiter_list_table *tab, uint32_t ident,
                 int64_t timeout_ns);

struct atomics_mutex *atomics_mutex_getptr(struct waiter_list_table *tab,
                                           uint32_t ident);
void atomics_mutex_lock(struct atomics_mutex *lock);
void atomics_mutex_unlock(struct atomics_mutex *lock);