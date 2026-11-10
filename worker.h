#ifndef WORKER_H
#define WORKER_H

typedef struct {
    int connected_socket;
    char directory[10];
} WorkerArgs;

void *Worker(void *argument);

#endif
