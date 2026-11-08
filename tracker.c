#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int ReadConfig(long *port, char directory[10])
{
    /* read sconfig */ 
    FILE *config = fopen("sconfig", "r");
    if (config == NULL) {
        perror("sconfig");
        return -1;
    }

    char extra;

    /* validate sconfig format */
    int fields = fscanf(config, "%ld %9s %c", port, directory, &extra);
    fclose(config);

    if (fields != 2) {
        fprintf(stderr, "sconfig needs a port number and a directory name.\n");
        return -1;
    }

    if (*port < 1 || *port > 65535) {
        fprintf(stderr, "The port must be a number from 1 to 65535.\n");
        return -1;
    }

    /* print port number and directory path*/
    printf("Tracker port: %ld\n", *port);
    printf("Tracker directory: %s\n", directory);
    return 0;
}

int ListenerStartup(void)
{
    long port;
    char directory[10];

    if (ReadConfig(&port, directory) == -1) {
        return -1;
    }

    /* ask for IPv4 TCP addresses suitable for a listening server. */
    struct addrinfo hints, *results, *address;
    char port_string[6];
    snprintf(port_string, sizeof port_string, "%ld", port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

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

        if (setsockopt(listening_socket, SOL_SOCKET, SO_REUSEADDR,
                       &reuse_address, sizeof reuse_address) == -1) {
            perror("setsockopt");
            close(listening_socket);
            freeaddrinfo(results);
            return -1;
        }

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

    /* allow connection requests to wait until the server accepts them. */
    if (listen(listening_socket, 10) == -1) {
        perror("listen");
        close(listening_socket);
        return -1;
    }

    printf("Listening on port %ld. Ctrl+C to quit. \n", port);

    return listening_socket;
}

/* wait for one peer and return its connected socket. */
int WaitForPeer(int listening_socket)
{
    struct sockaddr_storage peer_address;
    socklen_t peer_address_size = sizeof peer_address;

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

/* handle one accepted connection. */
void *Worker(void *argument)
{
    int connected_socket = *(int *)argument;
    free(argument);

    printf("Worker handling socket %d.\n", connected_socket);

    /* Read and print a short test message from the peer. */
    char message[1024];
    ssize_t bytes_received = recv(connected_socket, message, sizeof message - 1, 0);
    if (bytes_received > 0) {
        message[bytes_received] = '\0';
        printf("Received: %s\n", message);
    } else if (bytes_received == 0) {
        printf("Peer disconnected without sending a message.\n");
    } else {
        perror("recv");
    }

    close(connected_socket);
    return NULL;
}

int main(void)
{
    int listening_socket = ListenerStartup();
    if (listening_socket == -1) {
        return 1; // Startup failure
    }

    /* accept connections one at a time and continue listening. */
    while (1) {
        int connected_socket = WaitForPeer(listening_socket);
        if (connected_socket == -1) {
            continue;
        }

        /* give worker its own copy of the connected socket number. */
        int *worker_socket = malloc(sizeof *worker_socket);
        if (worker_socket == NULL) {
            perror("malloc");
            close(connected_socket);
            continue;
        }
        *worker_socket = connected_socket;

        pthread_t worker_thread;
        int thread_status = pthread_create(&worker_thread, NULL, Worker,
                                           worker_socket);
        if (thread_status != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(thread_status));
            free(worker_socket);
            close(connected_socket);
            continue;
        }

        /* release thread resources automatically when the worker finishes. */
        thread_status = pthread_detach(worker_thread);
        if (thread_status != 0) {
            fprintf(stderr, "pthread_detach: %s\n", strerror(thread_status));
        }
    }
}
