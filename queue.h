struct node {
    int fd;
    struct node *next;
};

struct queue {
    struct node *first;
    struct node *last;
};

struct queue *new_queue();
void enqueue(queue *queue, int fd);
int dequeue(queue *queue);
int queue_is_empty(queue *queue);
