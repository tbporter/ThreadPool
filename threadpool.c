#include <pthread.h>
#include <stdbool.h>
#include "list.h"

void* (* thread_running)(void* pool);

struct future {
    struct list_elem elem;
    void* result;
    void* argument;
    thread_pool_callable_t execution;
    pthread_mutex_t lock;
};

struct thread_data {
    struct list_elem elem;
    pthread_t thread;
}

struct thread_pool {
    struct list work_queue;
    struct list thread_list;
    bool running;
    pthread_cond_t condition;
    pthread_mutex_t lock;
}

struct thread_pool* thread_pool_new(int nthreads) {
    struct thread_pool* pool;
    if ((pool = malloc(sizeof(struct thread_pool))) == -1) {
        perror("Error in malloc\n");
        return NULL;
    }
    pool->condition = PTHREAD_CONT_INITIALIZER;
    pool->lock = PTHREAD_MUTEX_INITIALIZER;
    list_init(&pool->work_queue);
    list_init(&pool->thread_list);
    pool->running = true;

    if (pthread_mutex_lock(pool->lock) == -1) {
        perror("Error locking thread pool mutex the first time\n");
        return NULL;
    }

    int i;
    for (i = 0; i < nthreads; i++) {
        struct thread_data* tdata;
        if ((tdata = malloc(sizeof(struct thread_data))) == -1) {
            perror("Error in malloc\n");
            return NULL;
        }
        if (pthread_create(&tdata->thread, NULL, thread_running, (void *) pool) == -1) {
            perror("Error spawning threads for threadpool\n");
            return NULL;
        }
        list_push_back(&pool->thread_list, tdata->elem);
    }
    if (pthread_mutex_unlock(pool->lock) == -1) {
        perror("Error unlocking thread pool mutex the first time\n");
        return NULL;
    }
}

void* thread_running(void* pool) {
    struct thread_pool* pool = (struct thread_pool*) pool;
    struct future* future = NULL;
    /* Race conditions don't apply to checking running state, but the loop is cleaner like this */
    if (pthread_mutex_lock(pool->lock) == -1) {
        perror("Error locking thread pool mutex in worker thread\n");
        return NULL;
    }
    while (pool->running) {
        /* Make sure the mutex is unlocked */
        if (!list_empty(pool->work_queue)) {
            future = list_entry(list_pop_front(&pool->work_queue), struct future, elem);
            if (pthread_mutex_unlock(pool->lock) == -1) {
                perror("Error unlocking thread pool mutex in worker thread\n");
                return NULL;
            }
            /* Execute the future now */
            future->result = future->execution(future->argument);
            /* Signal future */
        }
        else {
            if (pthread_mutex_unlock(pool->lock) == -1) {
                perror("Error unlocking thread pool mutex in worker thread\n");
                return NULL;
            }
            /* It's empty so lets wait */
            pthread_cond_wait(pool->cond, pool->lock);
            /* loop assumes thread pool lock is possessed when starting so lets just reloop */
        }
    }
    if (pthread_mutex_unlock(pool->lock) == -1) {
        perror("Error unlocking thread pool mutex in worker thread\n");
        return NULL;
    }
}



