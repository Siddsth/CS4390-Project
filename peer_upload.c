#define _POSIX_C_SOURCE 200809L
#include "peer.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int listen_fd;
    char shared_folder[256];
} SeedListener;

static int OpenSeedListener(int listen_port)
{
    struct sockaddr_in seed_addr = {0};
    seed_addr.sin_family = AF_INET;
    seed_addr.sin_port = htons(listen_port);
    seed_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return -1;
    }

    /* Allow the seed to restart between local tests. */
    int reuse_addr = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_addr, sizeof reuse_addr) == -1) {
        perror("setsockopt");
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&seed_addr, sizeof seed_addr) == -1) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 10) == -1) {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

static FILE *OpenSeedFile(char *request, size_t request_len, const char *shared_folder)
{
    if (request_len > 0 && request[request_len - 1] == '\n') {
        request_len--;
        request[request_len] = '\0';
    }
    if (request_len > 0 && request[request_len - 1] == '\r') {
        request_len--;
        request[request_len] = '\0';
    }

    if (request_len < 7 || strlen(request) != request_len ||
        strncmp(request, "<GET ", 5) != 0 || request[request_len - 1] != '>') {
        fprintf(stderr, "Use <GET filename> for a seed request.\n");
        return NULL;
    }

    /* Removing the closing bracket leaves the filename after "<GET ". */
    request[request_len - 1] = '\0';
    const char *filename = request + 5;
    if (strlen(filename) > 249 || strpbrk(filename, "/<> \t\r\n") != NULL ||
        strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        fprintf(stderr, "Use a single shared filename without spaces or path separators.\n");
        return NULL;
    }

    char shared_path[512];
    snprintf(shared_path, sizeof shared_path, "%s/%s", shared_folder, filename);
    FILE *shared_file = fopen(shared_path, "rb");
    if (shared_file == NULL) {
        perror(shared_path);
    } else {
        printf("Opened shared file: %s\n", shared_path);
    }
    return shared_file;
}

static void *ListenForDownloads(void *argument)
{
    SeedListener seed = *(SeedListener *)argument;
    free(argument);

    while (1) {
        /* Only the accepted connection is closed after each request. */
        int downloader_fd = accept(seed.listen_fd, NULL, NULL);
        if (downloader_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            close(seed.listen_fd);
            return NULL;
        }
        printf("Downloader connected.\n");

        char seed_request[TRACKER_LINE_SIZE];
        /* One read for this basic test. */
        ssize_t n_read = recv(downloader_fd, seed_request,
                              sizeof seed_request - 1, 0);
        if (n_read < 0) {
            perror("recv");
        } else if (n_read == 0) {
            printf("Downloader closed without sending a request.\n");
        } else {
            seed_request[n_read] = '\0';
            printf("Seed received: %s", seed_request);
            if (seed_request[n_read - 1] != '\n') {
                putchar('\n');
            }

            FILE *shared_file = OpenSeedFile(seed_request, (size_t)n_read,
                                             seed.shared_folder);
            if (shared_file != NULL) {
                char file_data[1024];
                size_t file_bytes = fread(file_data, 1, sizeof file_data, shared_file);
                if (ferror(shared_file)) {
                    perror("read shared file");
                } else {
                    size_t bytes_sent = 0;
                    while (bytes_sent < file_bytes) {
                        ssize_t n_sent = send(downloader_fd, file_data + bytes_sent,
                                              file_bytes - bytes_sent, MSG_NOSIGNAL);
                        if (n_sent < 0 && errno == EINTR) {
                            continue;
                        }
                        if (n_sent <= 0) {
                            perror("send file");
                            break;
                        }
                        bytes_sent += (size_t)n_sent;
                    }
                    if (bytes_sent == file_bytes) {
                        printf("Sent %zu file bytes.\n", file_bytes);
                    }
                }
                fclose(shared_file);
            }
        }
        close(downloader_fd);
        fflush(stdout);
    }
}

int StartSeedListener(const PeerSettings *seed)
{
    /* Bind before starting the thread so port errors stop startup immediately. */
    int listen_fd = OpenSeedListener(seed->listen_port);
    if (listen_fd == -1) {
        return -1;
    }

    SeedListener *listener_arg = malloc(sizeof *listener_arg);
    if (listener_arg == NULL) {
        perror("malloc");
        close(listen_fd);
        return -1;
    }
    listener_arg->listen_fd = listen_fd;
    strcpy(listener_arg->shared_folder, seed->shared_folder);

    pthread_t seed_thread;
    int thread_error = pthread_create(&seed_thread, NULL, ListenForDownloads,
                                      listener_arg);
    if (thread_error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(thread_error));
        free(listener_arg);
        close(listen_fd);
        return -1;
    }

    /* The listener owns its socket and runs until the peer exits. */
    thread_error = pthread_detach(seed_thread);
    if (thread_error != 0) {
        fprintf(stderr, "pthread_detach: %s\n", strerror(thread_error));
        return -1;
    }

    printf("Seed listening on port %d. Shared folder: %s.\n",
           seed->listen_port, seed->shared_folder);
    fflush(stdout);
    return 0;
}
