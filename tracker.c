/* Expose POSIX declarations such as getaddrinfo() before including headers. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "worker.h"

/* port and directory are outputs. The caller supplies room for 10 characters. */
static int ReadConfig(long *port, char directory[10])
{
    /* read sconfig */ 
    FILE *config = fopen("sconfig", "r");
    if (config == NULL) {
        perror("sconfig");
        return -1;
    }

    char extra;

    /* %ld fills the caller's long. %9s leaves one byte for the string's '\0'.
       The space before %c skips whitespace, so a third assignment means
       there was extra content after the two expected fields. */
    int fields = fscanf(config, "%ld %9s %c", port, directory, &extra);
    fclose(config);

    if (fields != 2) {
        fprintf(stderr, "sconfig needs a port number and a directory name.\n");
        return -1;
    }

    /* *port reads the value that fscanf() wrote through the pointer. */
    if (*port < 1 || *port > 65535) {
        fprintf(stderr, "The port must be a number from 1 to 65535.\n");
        return -1;
    }

    /* print port number and directory path*/
    printf("Tracker port: %ld\n", *port);
    printf("Tracker directory: %s\n", directory);
    return 0;
}

static int ListenerStartup(char directory[10])
{
    long port;

    if (ReadConfig(&port, directory) == -1) {
        return -1;
    }

    /* getaddrinfo() takes the port as text. Five digits plus '\0' fit here
       because ReadConfig() already checked the port range. */
    struct addrinfo hints, *results, *address;
    char port_string[6];
    snprintf(port_string, sizeof port_string, "%ld", port);

    /* Zero every hint first. AF_INET selects IPv4, SOCK_STREAM selects TCP
       here, and AI_PASSIVE with a NULL host gives a wildcard bind address. */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    /* getaddrinfo() supplies its own error codes and allocates the result list. */
    int status = getaddrinfo(NULL, port_string, &hints, &results);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    /* try each returned address until a socket can be created and bound. */
    int listening_socket = -1;
    int reuse_address = 1;
    for (address = results; address != NULL; address = address->ai_next) {
        listening_socket = socket(address->ai_family, address->ai_socktype,
                                  address->ai_protocol);
        if (listening_socket == -1) {
            continue;
        }

        /* SO_REUSEADDR helps the server bind again soon after a restart.
           The option value is passed by address along with its byte size. */
        if (setsockopt(listening_socket, SOL_SOCKET, SO_REUSEADDR,
                       &reuse_address, sizeof reuse_address) == -1) {
            perror("setsockopt");
            close(listening_socket);
            freeaddrinfo(results);
            return -1;
        }

        /* bind() assigns this socket the local address and port. On failure,
           close this socket before trying the next candidate in the list. */
        if (bind(listening_socket, address->ai_addr, address->ai_addrlen) == -1) {
            perror("bind");
            close(listening_socket);
            listening_socket = -1;
            continue;
        }

        break;
    }
    freeaddrinfo(results);

    if (listening_socket == -1) {
        fprintf(stderr, "Could not create and bind a listening socket.\n");
        return -1;
    }

    printf("Socket bound to port %ld.\n", port);

    /* listen() marks this socket as a listener. The second argument is the
       requested backlog for connections waiting to be accepted. */
    if (listen(listening_socket, 10) == -1) {
        perror("listen");
        close(listening_socket);
        return -1;
    }

    printf("Listening on port %ld. Ctrl+C to quit. \n", port);

    return listening_socket;
}

/* wait for one peer and return its connected socket. */
static int WaitForPeer(int listening_socket)
{
    /* accept() writes the remote address into this buffer. Its length starts
       as the available space and comes back as the size actually written. */
    struct sockaddr_storage peer_address;
    socklen_t peer_address_size = sizeof peer_address;

    /* accept() can block until a peer connects. It returns a separate socket
       for talking to that peer while listening_socket stays open. */
    int connected_socket = accept(listening_socket,
                                  (struct sockaddr *)&peer_address,
                                  &peer_address_size);
    if (connected_socket == -1) {
        perror("accept");
        return -1;
    }

    printf("Peer connected.\n");
    return connected_socket;
}

/* Start one worker; it owns the socket after a successful pthread_create. */
static void StartWorker(int connected_socket, const char *directory)
{
    /* Allocate one argument block per connection so it outlives this call.
       sizeof *worker_args gets the size of the struct, rather than the pointer. */
    WorkerArgs *worker_args = malloc(sizeof *worker_args);
    if (worker_args == NULL) {
        perror("malloc");
        close(connected_socket);
        return;
    }
    worker_args->connected_socket = connected_socket;
    strcpy(worker_args->directory, directory);

    /* pthread_create() starts Worker(worker_args). It returns an error code
       directly, so strerror() uses thread_status here rather than errno. */
    pthread_t worker_thread;
    int thread_status = pthread_create(&worker_thread, NULL, Worker,
                                       worker_args);
    if (thread_status != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(thread_status));
        free(worker_args);
        close(connected_socket);
        return;
    }

    /* Detach so the thread's resources are reclaimed when it exits without
       main needing to wait for it with pthread_join(). */
    thread_status = pthread_detach(worker_thread);
    if (thread_status != 0) {
        fprintf(stderr, "pthread_detach: %s\n", strerror(thread_status));
    }
}

int main(void)
{
    char directory[10];
    int listening_socket = ListenerStartup(directory);
    if (listening_socket == -1) {
        return 1; // Startup failure
    }

    /* The main thread accepts connections. Each worker handles its own peer,
       so the main thread can accept another connection while it runs. */
    while (1) {
        int connected_socket = WaitForPeer(listening_socket);
        if (connected_socket == -1) {
            continue;
        }

        StartWorker(connected_socket, directory);
    }
}
