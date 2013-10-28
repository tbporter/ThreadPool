#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <semaphore.h>

#include "threadpool.h"
#include "list.h"

void* thread_run(void* tpool);

struct future {
    struct list_elem elem;
    void* result;
    void* argument;
    thread_pool_callable_func_t execution;
    sem_t sem;
};

struct thread_data {
    struct list_elem elem;
    pthread_t thread;
};

struct thread_pool {
    struct list work_queue;
    struct list thread_list;
    bool running;
    pthread_cond_t condition;
    pthread_mutex_t lock;
};

struct thread_pool* thread_pool_new(int nthreads) {
    struct thread_pool* pool;
    if ((long) (pool = malloc(sizeof(struct thread_pool))) == -1) {
        perror("Error in malloc\n");
        return NULL;
    }
    if (pthread_cond_init(&pool->condition, NULL) == -1) {
        perror("Error initializing threadpool condition\n");
        return NULL;
    }
    if (pthread_mutex_init(&pool->lock, NULL) == -1) {
        perror("Error initializing threadpool mutex\n");
        return NULL;
    }
    list_init(&pool->work_queue);
    list_init(&pool->thread_list);
    pool->running = true;

    if (pthread_mutex_lock(&pool->lock) == -1) {
        perror("Error locking thread pool mutex the first time\n");
        return NULL;
    }

    int i;
    for (i = 0; i < nthreads; i++) {
        struct thread_data* tdata;
        if ((long) (tdata = malloc(sizeof(struct thread_data))) == -1) {
            perror("Error in malloc\n");
            return NULL;
        }
        if (pthread_create(&tdata->thread, NULL, thread_run, (void *) pool) == -1) {
            perror("Error spawning threads for threadpool\n");
            return NULL;
        }
        list_push_back(&pool->thread_list, &tdata->elem);
    }
    if (pthread_mutex_unlock(&pool->lock) == -1) {
        perror("Error unlocking thread pool mutex the first time\n");
        return NULL;
    }
    return pool;
}

void* thread_run(void* tpool) {
    struct thread_pool* pool = (struct thread_pool*) pool;
    struct future* future = NULL;
    /* Race conditions don't apply to checking running state, but the loop is cleaner like this */
    if (pthread_mutex_lock(&pool->lock) == -1) {
        perror("Error locking thread pool mutex in worker thread\n");
        return NULL;
    }
    while (pool->running) {
        if (!list_empty(&pool->work_queue)) {
            future = list_entry(list_pop_front(&pool->work_queue), struct future, elem);
            if (pthread_mutex_unlock(&pool->lock) == -1) {
                perror("Error unlocking thread pool mutex in worker thread\n");
                return NULL;
            }
            /* Execute the future now */
            future->result = future->execution(future->argument);
            sem_post(&(future->sem));

            /* We've finished there might be more in the queue */
            if (pthread_mutex_lock(&pool->lock) == -1) {
                perror("Error locking thread pool mutex in worker thread\n");
                return NULL;
            }
        }
        else {
            if (pthread_mutex_unlock(&pool->lock) == -1) {
                perror("Error unlocking thread pool mutex in worker thread\n");
                return NULL;
            }
            /* It's empty so lets wait */
            pthread_cond_wait(&pool->condition, &pool->lock);
            /* loop assumes thread pool lock is possessed when starting so lets just reloop */
        }
    }
    /* Loop starts with it locked, make sure we unlock the pool */
    if (pthread_mutex_unlock(&pool->lock) == -1) {
        perror("Error unlocking thread pool mutex in worker thread\n");
        return NULL;
    }
    return NULL;
}

void future_free(struct future* f){
    free(f);
    
    return;
}

void* future_get(struct future* f){
    /* Wait until the future is done */
    sem_wait(&(f->sem));

    return f->result;
}

void thread_pool_shutdown(struct thread_pool* tpool){
    if(tpool == NULL)
        return;

    if(pthread_mutex_lock(&(tpool->lock)) == -1){
        perror("Error locking thread pool mutex in shutdown\n");
    }

    tpool->running = false;

    /* TODO: more stuff here */

    if(pthread_mutex_destroy(&(tpool->lock)) == -1){
        perror("Error destroying thread pool mutex in shutdown\n");
    }

    if(pthread_cond_destroy(&(tpool->condition)) == -1){
        perror("Error destroying thread pool condition in shutdown\n");
    }

    free(tpool);
    
    return;
}