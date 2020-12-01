#include <err.h>
#include <iostream>
#include <pthread.h>
#include <stdlib.h>

#include "queue.h"

extern pthread_cond_t condl;
extern pthread_mutex_t mutex;

/*
 * Initializes a new queue and returns it
 */
struct queue *new_queue()
{
    struct queue *queue = (struct queue *)malloc(sizeof(struct queue));
    queue->first = NULL;
    queue->last = NULL;
    return queue;
}

/*
 * Add a new file descriptor to the queue
 */
void enqueue(queue *queue, int fd)
{
    struct node *new_node = (struct node *)malloc(sizeof(struct node));
    new_node->fd = fd;
    new_node->next = NULL;
    pthread_mutex_lock(&mutex); // mutex lock: only one thread can add to the queue at once
    if(queue->last == NULL) {
        queue->first = new_node;
        queue->last = new_node;
    } else {
        queue->last->next = new_node;
        queue->last = new_node;
    }
    pthread_mutex_unlock(&mutex);
    pthread_cond_signal(&condl);
}

/*
 * Pop a file descriptor off of the queue
 */
int dequeue(queue *queue)
{
    int fd;

    pthread_mutex_lock(&mutex); // lock so only one thread can dequeue at once

    while(queue_is_empty(queue)) {
        printf("Worker thread %lu is waiting\n", pthread_self());
        if(pthread_cond_wait(&condl, &mutex) != 0) {
            err(1, NULL);
        }
    }

    struct node *tmp = queue->first;
    fd = queue->first->fd;
    free(tmp);
    queue->first = queue->first->next;

    if(queue->first == NULL) {
        queue->last = NULL;
    }
    pthread_mutex_unlock(&mutex);

    return fd;
}

/*
 * Determien if a queue is empty
 */
int queue_is_empty(queue *queue)
{
    return queue->first == NULL;
}
