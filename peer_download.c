#define _POSIX_C_SOURCE 200809L
#include "peer.h"
#include "file_hash.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    char filename[250];
    unsigned long long file_size;
    char md5[33];
    char seed_ip[INET_ADDRSTRLEN];
    int seed_port;
    long long last_seen;
} DownloadInfo;


/* Both the tracker and a seed are reached by an IPv4 address and TCP port. */
static int OpenTcpConnection(const char *remote_ip, int remote_port)
{
    if (remote_port < 1 || remote_port > 65535) {
        fprintf(stderr, "The remote port must be from 1 to 65535.\n");
        return -1;
    }

    struct sockaddr_in remote_addr = {0};
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, remote_ip, &remote_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid remote IPv4 address.\n");
        return -1;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        perror("socket");
        return -1;
    }

    if (connect(socket_fd, (struct sockaddr *)&remote_addr, sizeof remote_addr) == -1) {
        perror("connect");
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

/* Tracker commands and seed requests send text lines. */
static int SendPeerLine(int remote_fd, const char *outgoing)
{
    size_t bytes_left = strlen(outgoing);
    while (bytes_left > 0) {
        /* Report a disconnect here instead of exiting on SIGPIPE. */
        ssize_t n_sent = send(remote_fd, outgoing, bytes_left, MSG_NOSIGNAL);
        if (n_sent < 0 && errno == EINTR) {
            continue;
        }
        if (n_sent <= 0) {
            perror("send");
            return -1;
        }

        outgoing += n_sent;
        bytes_left -= (size_t)n_sent;
    }
    return 0;
}

/* Test files fit in one 1024-byte buffer and use one receive. */
static int DownloadSeedFile(const char *seed_ip, int seed_port, const char *filename,
                            const char *save_path, const DownloadInfo *expected)
{
    if (filename[0] == '\0' || strlen(filename) > 249 ||
        strpbrk(filename, "/<> \t\r\n") != NULL ||
        strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        fprintf(stderr, "Use a single shared filename without spaces or path separators.\n");
        return -1;
    }

    char seed_cmd[TRACKER_LINE_SIZE];
    int cmd_len = snprintf(seed_cmd, sizeof seed_cmd, "<GET %s>\n", filename);
    if (cmd_len < 0 || (size_t)cmd_len >= sizeof seed_cmd) {
        fprintf(stderr, "The seed request is too long.\n");
        return -1;
    }

    int seed_fd = OpenTcpConnection(seed_ip, seed_port);
    if (seed_fd == -1) {
        return -1;
    }

    if (SendPeerLine(seed_fd, seed_cmd) == -1) {
        close(seed_fd);
        return -1;
    }

    char file_data[1024];
    ssize_t n_read = recv(seed_fd, file_data, sizeof file_data, 0);
    if (n_read < 0) {
        perror("recv file");
        close(seed_fd);
        return -1;
    }
    close(seed_fd);
    if (n_read == 0 && (expected == NULL || expected->file_size != 0)) {
        fprintf(stderr, "Seed closed without sending file data.\n");
        return -1;
    }

    if (expected != NULL) {
        char md5[33];
        if ((unsigned long long)n_read != expected->file_size ||
            GetMD5(file_data, (size_t)n_read, md5) == -1 ||
            strcasecmp(md5, expected->md5) != 0) {
            fprintf(stderr, "Downloaded data does not match the tracker size or MD5.\n");
            return -1;
        }
    }

    FILE *downloaded_file = fopen(save_path, "wb");
    if (downloaded_file == NULL) {
        perror(save_path);
        return -1;
    }
    size_t n_written = fwrite(file_data, 1, (size_t)n_read, downloaded_file);
    if (n_written != (size_t)n_read) {
        perror("write downloaded file");
        fclose(downloaded_file);
        return -1;
    }
    if (fclose(downloaded_file) == EOF) {
        perror("close downloaded file");
        return -1;
    }

    printf("Saved %zd bytes to %s.\n", n_read, save_path);
    return 0;
}

int DownloadFromSeed(const char *seed_ip, int seed_port, const char *filename)
{
    return DownloadSeedFile(seed_ip, seed_port, filename, filename, NULL);
}

static int ReadDownloadInfo(const char *tracker_text, const char *track_name, DownloadInfo *file)
{
    *file = (DownloadInfo){0};
    file->last_seen = -1;
    char *copy = strdup(tracker_text);
    if (copy == NULL) {
        return -1;
    }

    int have_size = 0;
    char *parse_pos;
    char *row = strtok_r(copy, "\n", &parse_pos);
    while (row != NULL) {
        if (row[0] != '#') {
            sscanf(row, "Filename: %249s", file->filename);
            sscanf(row, "MD5: %32s", file->md5);
            char size_text[32];
            if (sscanf(row, "Filesize: %31s", size_text) == 1) {
                if (strspn(size_text, "0123456789") != strlen(size_text)) {
                    free(copy);
                    return -1;
                }
                errno = 0;
                file->file_size = strtoull(size_text, NULL, 10);
                if (errno == ERANGE) {
                    free(copy);
                    return -1;
                }
                have_size = 1;
            }

            char ip[INET_ADDRSTRLEN];
            unsigned int port;
            unsigned long long start, end;
            long long seen;
            struct in_addr address;
            if (have_size && sscanf(row, "%15[^:]:%u:%llu:%llu:%lld",
                    ip, &port, &start, &end, &seen) == 5 &&
                inet_pton(AF_INET, ip, &address) == 1 && port >= 1 && port <= 65535 &&
                start == 0 && end >= file->file_size && seen > file->last_seen) {
                strcpy(file->seed_ip, ip);
                file->seed_port = (int)port;
                file->last_seen = seen;
            }
        }
        row = strtok_r(NULL, "\n", &parse_pos);
    }
    free(copy);

    char expected_name[256];
    snprintf(expected_name, sizeof expected_name, "%s.track", file->filename);
    if (!have_size || file->filename[0] == '\0' ||
        strpbrk(file->filename, "/<> \t\r\n") != NULL ||
        strcmp(file->filename, ".") == 0 || strcmp(file->filename, "..") == 0 ||
        strcmp(expected_name, track_name) != 0 || strlen(file->md5) != 32 ||
        strspn(file->md5, "0123456789abcdefABCDEF") != 32 || file->seed_port == 0) {
        fprintf(stderr, "Tracker metadata is incomplete or has no complete seed.\n");
        return -1;
    }
    return 0;
}

static int ReceiveTrackerFile(FILE *tracker_stream, char **text, size_t *text_bytes)
{
    *text = NULL;
    *text_bytes = 0;
    char *row = NULL;
    size_t row_capacity = 0;
    char expected_md5[33] = "";
    int got_end = 0;
    ssize_t row_bytes;

    while ((row_bytes = getline(&row, &row_capacity, tracker_stream)) != -1) {
        fputs(row, stdout);
        if (strlen(row) != (size_t)row_bytes) {
            break;
        }
        if (strncmp(row, "<REP GET END ", 13) == 0) {
            if (row_bytes == 47 && row[45] == '>' && row[46] == '\n') {
                memcpy(expected_md5, row + 13, 32);
                expected_md5[32] = '\0';
                got_end = strspn(expected_md5, "0123456789abcdefABCDEF") == 32;
            }
            break;
        }

        char *larger_text = realloc(*text, *text_bytes + (size_t)row_bytes + 1);
        if (larger_text == NULL) {
            break;
        }
        *text = larger_text;
        memcpy(*text + *text_bytes, row, (size_t)row_bytes);
        *text_bytes += (size_t)row_bytes;
        (*text)[*text_bytes] = '\0';
    }
    free(row);

    char actual_md5[33];
    if (!got_end || *text == NULL || fgetc(tracker_stream) != EOF ||
        ferror(tracker_stream) || GetMD5(*text, *text_bytes, actual_md5) == -1 ||
        strcasecmp(actual_md5, expected_md5) != 0) {
        fprintf(stderr, "Tracker transfer is incomplete or its MD5 does not match.\n");
        free(*text);
        *text = NULL;
        return -1;
    }
    printf("Tracker MD5 verified.\n");
    return 0;
}

static int DownloadTrackedFile(const PeerSettings *peer, const char *track_name,
                               const char *tracker_text, size_t text_bytes)
{
    DownloadInfo file;
    if (ReadDownloadInfo(tracker_text, track_name, &file) == -1) {
        return -1;
    }

    if (mkdir("tracker_cache", 0777) == -1 && errno != EEXIST) {
        perror("tracker_cache");
        return -1;
    }
    char cache_path[512];
    snprintf(cache_path, sizeof cache_path, "tracker_cache/%s", track_name);
    FILE *cache = fopen(cache_path, "wb");
    if (cache == NULL) {
        perror(cache_path);
        return -1;
    }
    size_t written = fwrite(tracker_text, 1, text_bytes, cache);
    int close_status = fclose(cache);
    if (written != text_bytes || close_status != 0) {
        perror("save tracker cache");
        return -1;
    }

    printf("Downloading %s from %s:%d.\n", file.filename, file.seed_ip, file.seed_port);
    char save_path[512];
    snprintf(save_path, sizeof save_path, "%s/%s", peer->shared_folder, file.filename);
    if (DownloadSeedFile(file.seed_ip, file.seed_port, file.filename, save_path, &file) == -1) {
        return -1;
    }
    printf("File MD5 verified. Download complete.\n");
    if (unlink(cache_path) == -1) {
        perror("remove completed tracker cache");
        return -1;
    }
    return 0;
}

/* One command per connection: the tracker closes it when the reply is finished. */
static int HandleTrackerReply(FILE *tracker_stream, const PeerSettings *peer,
                              const char *track_name)
{
    char reply_line[TRACKER_LINE_SIZE];
    int add_newline = 0;
    while (fgets(reply_line, sizeof reply_line, tracker_stream) != NULL) {
        fputs(reply_line, stdout);
        fflush(stdout);

        /* Status tokens such as succ may arrive without a trailing newline. */
        add_newline = strchr(reply_line, '\n') == NULL;

        if (strcmp(reply_line, "<REP GET BEGIN>\n") == 0) {
            if (track_name == NULL) {
                return -1;
            }
            char *tracker_text;
            size_t text_bytes;
            if (ReceiveTrackerFile(tracker_stream, &tracker_text, &text_bytes) == -1) {
                return -1;
            }
            int download_status = DownloadTrackedFile(peer, track_name, tracker_text, text_bytes);
            free(tracker_text);
            return download_status;
        }
    }

    if (ferror(tracker_stream)) {
        perror("read tracker reply");
        return -1;
    }
    if (add_newline) {
        putchar('\n');
    }
    return 0;
}

int RunTrackerClient(const PeerSettings *peer)
{
    printf("Tracker: %s:%d\n", peer->tracker_ip, peer->tracker_port);
    printf("Configured peer port: %d. Shared folder: %s\n",
           peer->listen_port, peer->shared_folder);
    while (1) {
        printf("Enter a tracker request (quit to exit): ");
        fflush(stdout);

        char tracker_cmd[TRACKER_LINE_SIZE];
        if (fgets(tracker_cmd, sizeof tracker_cmd, stdin) == NULL) {
            if (ferror(stdin)) {
                perror("read command");
                return -1;
            }
            return 0;
        }
        if (strcmp(tracker_cmd, "quit\n") == 0 ||
            strcmp(tracker_cmd, "quit\r\n") == 0) {
            return 0;
        }

        /* Discard the rest of a cut-off line before accepting another command. */
        if (strchr(tracker_cmd, '\n') == NULL) {
            fprintf(stderr, "Enter a complete request followed by Enter.\n");
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {
            }
            continue;
        }

        int tracker_fd = OpenTcpConnection(peer->tracker_ip, peer->tracker_port);
        if (tracker_fd == -1) {
            continue;
        }

        /* Closing the reply stream also closes this command's tracker socket. */
        FILE *tracker_stream = fdopen(tracker_fd, "r");
        if (tracker_stream == NULL) {
            perror("fdopen");
            close(tracker_fd);
            continue;
        }

        int exchange_status = SendPeerLine(tracker_fd, tracker_cmd);
        if (exchange_status == 0) {
            char *parse_pos;
            char *command = strtok_r(tracker_cmd, "<> \t\r\n", &parse_pos);
            const char *track_name = NULL;
            if (command != NULL && strcmp(command, "GET") == 0) {
                track_name = strtok_r(NULL, "<> \t\r\n", &parse_pos);
            }
            exchange_status = HandleTrackerReply(tracker_stream, peer, track_name);
        }
        fclose(tracker_stream);
        if (exchange_status == -1) {
            fprintf(stderr, "Tracker request failed.\n");
        }
    }
}
