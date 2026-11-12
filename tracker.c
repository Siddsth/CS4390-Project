#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "worker.h"

static int LoadTrackerSettings(long *tracker_port, char torrent_dir[10])
{
    FILE *sconfig = fopen("sconfig", "r");
    if (sconfig == NULL) {
        perror("sconfig");
        return -1;
    }

    char trailing;

    /* The directory must also fit in PeerSession.torrent_dir. */
    int fields_read = fscanf(sconfig, "%ld %9s %c", tracker_port,
                             torrent_dir, &trailing);
    fclose(sconfig);

    if (fields_read != 2) {
        fprintf(stderr, "sconfig needs a port number and a directory name.\n");
        return -1;
    }

    if (*tracker_port < 1 || *tracker_port > 65535) {
        fprintf(stderr, "The port must be a number from 1 to 65535.\n");
        return -1;
    }

    printf("Tracker port: %ld\n", *tracker_port);
    printf("Tracker directory: %s\n", torrent_dir);
    return 0;
}

static int OpenTrackerListener(char torrent_dir[10])
{
    long tracker_port;

    if (LoadTrackerSettings(&tracker_port, torrent_dir) == -1) {
        return -1;
    }

    struct addrinfo hints, *bind_addrs, *candidate;
    char port_text[6];
    snprintf(port_text, sizeof port_text, "%ld", tracker_port);

    /* Peers may run on other machines, so bind all local IPv4 interfaces. */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int lookup_error = getaddrinfo(NULL, port_text, &hints, &bind_addrs);
    if (lookup_error != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(lookup_error));
        return -1;
    }

    int listen_fd = -1;
    int reuse_addr = 1;
    for (candidate = bind_addrs; candidate != NULL; candidate = candidate->ai_next) {
        listen_fd = socket(candidate->ai_family, candidate->ai_socktype,
                           candidate->ai_protocol);
        if (listen_fd == -1) {
            continue;
        }

        /* Allow tracker restarts between tests while old connections age out. */
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                       &reuse_addr, sizeof reuse_addr) == -1) {
            perror("setsockopt");
            close(listen_fd);
            freeaddrinfo(bind_addrs);
            return -1;
        }

        if (bind(listen_fd, candidate->ai_addr, candidate->ai_addrlen) == -1) {
            perror("bind");
            close(listen_fd);
            listen_fd = -1;
            continue;
        }

        break;
    }
    freeaddrinfo(bind_addrs);

    if (listen_fd == -1) {
        fprintf(stderr, "Could not create and bind a listening socket.\n");
        return -1;
    }

    printf("Socket bound to port %ld.\n", tracker_port);

    /* This backlog covers waiting connections, not the number of active peers. */
    if (listen(listen_fd, 10) == -1) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    printf("Listening on port %ld. Ctrl+C to quit. \n", tracker_port);

    return listen_fd;
}

static int AcceptPeer(int listen_fd)
{
    struct sockaddr_storage peer_addr;
    socklen_t addr_len = sizeof peer_addr;

    /* The worker gets this connection; the main thread keeps the listener. */
    int peer_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &addr_len);
    if (peer_fd == -1) {
        perror("accept");
        return -1;
    }

    printf("Peer connected.\n");
    return peer_fd;
}

static void StartPeerWorker(int peer_fd, const char *torrent_dir)
{
    /* Each peer needs its own session; its worker may start after this returns. */
    PeerSession *session = malloc(sizeof *session);
    if (session == NULL) {
        perror("malloc");
        close(peer_fd);
        return;
    }
    session->peer_fd = peer_fd;
    strcpy(session->torrent_dir, torrent_dir);

    pthread_t peer_thread;
    /* A successful start transfers session and peer_fd to ServeTrackerPeer. */
    int thread_error = pthread_create(&peer_thread, NULL, ServeTrackerPeer,
                                      session);
    if (thread_error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(thread_error));
        free(session);
        close(peer_fd);
        return;
    }

    /* Each peer handles one command; the accept loop never joins its worker. */
    thread_error = pthread_detach(peer_thread);
    if (thread_error != 0) {
        fprintf(stderr, "pthread_detach: %s\n", strerror(thread_error));
    }
}

int main(void)
{
    char torrent_dir[10];
    int listen_fd = OpenTrackerListener(torrent_dir);
    if (listen_fd == -1) {
        return 1;
    }

    while (1) {
        int peer_fd = AcceptPeer(listen_fd);
        if (peer_fd == -1) {
            continue;
        }

        StartPeerWorker(peer_fd, torrent_dir);
    }
}
