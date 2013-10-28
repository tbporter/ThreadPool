#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <semaphore.h>

#include "threadpool.h"
#include "list.h"

static void* thread_run(void* tpool);

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

static void* thread_run(void* tpool) {
    struct thread_pool* pool = (struct thread_pool*) tpool;
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
            fprintf(stderr, "Completed execution\n");
            sem_post(&(future->sem));
            /* We've finished there might be more in the queue */
            if (pthread_mutex_lock(&pool->lock) == -1) {
                perror("Error locking thread pool mutex in worker thread\n");
                return NULL;
            }
        }
        else {
            /* It's empty so lets wait, this also unlockes the mutex */
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


struct future * thread_pool_submit(struct thread_pool * pool,
        thread_pool_callable_func_t callable, void* callable_data) {
    struct future* fut;
    if ((long) (fut = malloc(sizeof(struct future))) == -1) {
        perror("Error locking thread pool mutex in thread pool submit\n");
        return NULL;
    }
    /* Set the memory values */
    fut->execution = callable;
    fut->argument = callable_data;
    /* Initialize the semaphore */
    if (sem_init(&fut->sem, 0, 0) == -1) {
        perror("Error initializing semaphore for future\n");
        return NULL;
    }
    /* Throw it into the work queue */
    if (pthread_mutex_lock(&pool->lock) == -1) {
        perror("Error locking thread pool mutex in thread pool submit\n");
        return NULL;
    }
    list_push_back(&pool->work_queue, &fut->elem);
    /* Let a thread know there is work if one is waiting */
    if (pthread_cond_signal(&pool->condition) == -1) {
        perror("Error while signalling other threads of new job\n");
        return NULL;
    }
    if (pthread_mutex_unlock(&pool->lock) == -1) {
        perror("Error locking thread pool mutex in thread pool submit\n");
        return NULL;
    }
    return fut;
}

void thread_pool_shutdown(struct thread_pool* pool) {
    /* Had to move this up here for helgrind */
    if (pthread_mutex_lock(&pool->lock) == -1) {
        perror("Error locking thread pool mutex in thread pool shutdown\n");
        return;
    }
    pool->running = false;
    pthread_cond_broadcast(&pool->condition);
    while (!list_empty(&pool->thread_list)) {
        struct thread_data* tdata = list_entry(list_pop_front(&pool->thread_list), struct thread_data, elem);
        void* ret;
        /* We got what we came for, unlock so threads can finish up */
        if (pthread_mutex_unlock(&pool->lock) == -1) {
            perror("Error unlocking thread pool mutex in thread pool shutdown\n");
            return;
        }
        /* Join it, then relock to continue the loop */
        if (pthread_join(tdata->thread, &ret) == -1) {
            perror("Could join thread.\n");
            return;
        }
        /* Free thread_data */
        free(tdata);
        if (pthread_mutex_lock(&pool->lock) == -1) {
            perror("Error locking thread pool mutex in thread pool shutdown\n");
            return;
        }
    }
    /* Should lock incase the calling program is mucking around with this */
    /* Abandon the remaining futures */
    while (!list_empty(&pool->work_queue)) {
        list_pop_front(&pool->work_queue);
    }
    if (pthread_mutex_unlock(&pool->lock) == -1) {
        perror("Error unlocking thread pool mutex in thread pool shutdown\n");
        return;
    }
    /* clean up syncronization constructs */
    if (pthread_cond_destroy(&pool->condition) == -1) {
        perror("Error destroying thread pool condition\n");
        return;
    }
    if (pthread_mutex_destroy(&pool->lock) == -1) {
        perror("Error destroying thread pool lock\n");
        return;
    }
    free(pool);
}

void future_free(struct future* f){
    if (sem_destroy(&f->sem) == -1) {
        perror("Error destroying future semaphore\n");
        return;
    }
    free(f);
}

void* future_get(struct future* f){
    /* Wait until the future is done */
    if (sem_wait(&(f->sem)) == -1) {
        perror("Error waiting on future semaphore\n");
        return NULL;
    }

    return f->result;
}
