#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "threadpool.h"

struct work_st *dequeue(struct _threadpool_st *tp);
void enqueue(struct _threadpool_st *, struct work_st *);
void free_queue(struct _threadpool_st *);

// Corrected locking order
void lock(pthread_mutex_t *mutex) {
    if (pthread_mutex_lock(mutex) != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }
}

void unlock(pthread_mutex_t *mutex) {
    if (pthread_mutex_unlock(mutex) != 0) {
        perror("pthread_mutex_unlock");
        exit(EXIT_FAILURE);
    }
}

threadpool *create_threadpool(int num_threads_in_pool) {
    threadpool *t_pool = (threadpool *)malloc(sizeof(threadpool));
    if (t_pool == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    t_pool->num_threads = num_threads_in_pool;
    t_pool->qsize = 0;
    t_pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * t_pool->num_threads);
    if (t_pool->threads == NULL) {
        perror("malloc");
        free(t_pool);
        exit(EXIT_FAILURE);
    }

    t_pool->qtail = t_pool->qhead = NULL;
    pthread_cond_init(&t_pool->q_empty, NULL);
    pthread_cond_init(&t_pool->q_not_empty, NULL);
    pthread_mutex_init(&t_pool->qlock, NULL);
    t_pool->shutdown = t_pool->dont_accept = 0;

    for (int i = 0; i < t_pool->num_threads; ++i) {
        if (pthread_create(t_pool->threads + i, NULL, do_work, (void *)t_pool) != 0) {
            perror("pthread_create");
            free(t_pool->threads);
            free(t_pool);
            exit(EXIT_FAILURE);
        }
    }

    return t_pool;
}

void dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg) {
    lock(&from_me->qlock); // Acquire lock
    if (from_me->dont_accept) {
        unlock(&from_me->qlock); // Release lock
        return;
    }
    struct work_st *work = (struct work_st *)malloc(sizeof(struct work_st));
    if (work == NULL) {
        perror("malloc");
        free_queue(from_me);
        free(from_me);
        exit(EXIT_FAILURE);
    }
    work->routine = dispatch_to_here;
    work->arg = arg;

    enqueue(from_me, work);
    pthread_cond_signal(&from_me->q_not_empty);
    unlock(&from_me->qlock); // Release lock
}

void *do_work(void *p) {
    struct _threadpool_st *t_pool = (struct _threadpool_st *)p;
    while (1) {
        lock(&t_pool->qlock); // Acquire lock
        while (t_pool->qsize == 0 && !t_pool->shutdown) {
            pthread_cond_wait(&t_pool->q_not_empty, &t_pool->qlock);
        }
        if (t_pool->shutdown) {
            unlock(&t_pool->qlock); // Release lock
            pthread_exit(NULL);
        }
        struct work_st *work = dequeue(t_pool);
        unlock(&t_pool->qlock); // Release lock

        if (work != NULL) {
            work->routine(work->arg);
            free(work->arg);
            free(work);
        }

        lock(&t_pool->qlock); // Acquire lock
        if (t_pool->dont_accept && t_pool->qsize == 0) {
            pthread_cond_signal(&t_pool->q_empty);
        }
        unlock(&t_pool->qlock); // Release lock
    }
}

void destroy_threadpool(threadpool *destroyme) {
    lock(&destroyme->qlock); // Acquire lock

    destroyme->dont_accept = 1;
    while (destroyme->qsize > 0) {
        pthread_cond_wait(&destroyme->q_empty, &destroyme->qlock);
    }
    destroyme->shutdown = 1;
    unlock(&destroyme->qlock); // Release lock
    pthread_cond_broadcast(&destroyme->q_not_empty);

    for (int i = 0; i < destroyme->num_threads; ++i) {
        pthread_join(destroyme->threads[i], NULL);
    }
    free(destroyme->threads);
    free(destroyme);
}

struct work_st *dequeue(struct _threadpool_st *tp) {
    if (tp == NULL || tp->qhead == NULL) {
        return NULL;
    }

    struct work_st *ret = tp->qhead;
    struct work_st *new_head = ret->next;
    tp->qhead = new_head;
    tp->qsize--;
    if (tp->qsize == 0) {
        tp->qtail = NULL;
    }
    return ret;
}

void enqueue(struct _threadpool_st *tp, struct work_st *work) {
    if (tp->qtail == NULL) {
        tp->qsize++;
        tp->qhead = tp->qtail = work;
    } else {
        tp->qtail->next = work;
        tp->qtail = work;
        tp->qsize++;
    }
}

void free_queue(struct _threadpool_st *tp) {
    struct work_st *head = tp->qhead;
    while (head != NULL) {
        struct work_st *next = head->next;
        free(head);
        head = next;
    }
}

