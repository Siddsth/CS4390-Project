#define _POSIX_C_SOURCE 200809L
#include "worker.h"
#include "tracker_files.h"
#include "file_hash.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TRACKER_CMD_SIZE 1024
#define TRACKER_CMD_FIELDS 7

/* Reply text may need several sends; a disconnected peer must not raise SIGPIPE. */
static int SendPeerReply(int peer_fd, const char *reply_text)
{
    size_t bytes_left = strlen(reply_text);
    while (bytes_left > 0) {
        ssize_t n_sent = send(peer_fd, reply_text, bytes_left, MSG_NOSIGNAL);
        if (n_sent < 0 && errno == EINTR) {
            continue;
        }
        if (n_sent <= 0) {
            return -1;
        }

        reply_text += n_sent;
        bytes_left -= (size_t)n_sent;
    }
    return 0;
}

static int SplitTrackerCommand(char *cmd, char *parts[TRACKER_CMD_FIELDS])
{
    cmd += strspn(cmd, " \t\r\n");
    if (cmd[0] == '<') {
        cmd++;
    }
    int n_parts = 0;

    /* Parser state stays with this peer; other workers tokenize their own commands. */
    char *parse_pos;
    char *part = strtok_r(cmd, " \t\r\n>", &parse_pos);
    while (part != NULL) {
        if (n_parts == TRACKER_CMD_FIELDS) {
            return -1;
        }
        parts[n_parts++] = part;

        part = strtok_r(NULL, " \t\r\n>", &parse_pos);
    }
    return n_parts;
}

/* Peers advertise a filename, not a path outside the tracker directory. */
static int ValidSharedName(const char *shared_name)
{
    return strlen(shared_name) <= 249 && strchr(shared_name, '/') == NULL &&
           strcmp(shared_name, ".") != 0 && strcmp(shared_name, "..") != 0;
}

/* Used for file sizes, byte offsets, and advertised ports: no signs or suffixes. */
static int ParseUnsigned(const char *token, unsigned long long *number)
{
    if (token[0] == '\0' || strlen(token) > 31 ||
        strspn(token, "0123456789") != strlen(token)) {
        return 0;
    }

    errno = 0;
    *number = strtoull(token, NULL, 10);
    return errno != ERANGE;
}

/* This is the advertised serving endpoint, not the tracker connection's source port. */
static int ValidPeerEndpoint(const char *peer_ip, const char *peer_port)
{
    struct in_addr ipv4;
    unsigned long long port_number;
    return inet_pton(AF_INET, peer_ip, &ipv4) == 1 &&
           strlen(peer_port) <= 5 && ParseUnsigned(peer_port, &port_number) &&
           port_number >= 1 && port_number <= 65535;
}

static const char *HandleSeedCreate(const char *torrent_dir,
                                    char *parts[], int n_parts)
{
    if (n_parts != 7) {
        return "fail";
    }

    CreateRequest seed = {
        .filename = parts[1],
        .file_size = parts[2],
        .description = parts[3],
        .md5 = parts[4],
        .ip = parts[5],
        .port = parts[6]
    };

    /* The tracker checks MD5 text only; file data never passes through it. */
    unsigned long long seed_bytes;
    if (!ValidSharedName(seed.filename) ||
        !ParseUnsigned(seed.file_size, &seed_bytes) ||
        strlen(seed.description) > 255 ||
        strlen(seed.md5) != 32 ||
        strspn(seed.md5, "0123456789abcdefABCDEF") != 32 ||
        !ValidPeerEndpoint(seed.ip, seed.port)) {
        return "fail";
    }

    return CreateSeedTracker(torrent_dir, &seed);
}

static const char *HandlePeerUpdate(const char *torrent_dir,
                                    char *parts[], int n_parts)
{
    if (n_parts != 6) {
        return "fail";
    }
    UpdateRequest peer_update = {
        .filename = parts[1],
        .start = parts[2],
        .end = parts[3],
        .ip = parts[4],
        .port = parts[5]
    };

    unsigned long long start, end;
    if (!ValidSharedName(peer_update.filename) ||
        !ParseUnsigned(peer_update.start, &start) ||
        !ParseUnsigned(peer_update.end, &end) ||
        start > end || !ValidPeerEndpoint(peer_update.ip, peer_update.port)) {
        return "fail";
    }
    return UpdatePeerEntry(torrent_dir, &peer_update);
}

static const char *HandleFileList(int peer_fd, const char *torrent_dir,
                                  char *parts[], int n_parts)
{
    if (n_parts != 2 || strcmp(parts[1], "LIST") != 0) {
        return "fail";
    }

    TrackerListing *files;
    size_t file_count;
    if (ReadTrackerList(torrent_dir, &files, &file_count) == -1) {
        return "fail";
    }

    /* The count and rows use the same copy of the tracker metadata. */
    char list_line[TRACKER_CMD_SIZE];
    snprintf(list_line, sizeof list_line, "<REP LIST %zu>\n", file_count);
    int send_status = SendPeerReply(peer_fd, list_line);
    for (size_t i = 0; i < file_count && send_status == 0; i++) {
        snprintf(list_line, sizeof list_line, "<%zu %s %s %s>\n",
                 i + 1, files[i].filename, files[i].file_size, files[i].md5);
        send_status = SendPeerReply(peer_fd, list_line);
    }
    if (send_status == 0) {
        send_status = SendPeerReply(peer_fd, "<REP LIST END>\n");
    }
    free(files);
    if (send_status == -1) {
        fprintf(stderr, "Could not send tracker list.\n");
    }
    return NULL;
}

static const char *HandleTrackerGet(int peer_fd, const char *torrent_dir,
                                    char *parts[], int n_parts)
{
    if (n_parts != 2) {
        return "fail";
    }
    size_t name_len = strlen(parts[1]);
    if (name_len <= 6 || name_len > 255 ||
        strcmp(parts[1] + name_len - 6, ".track") != 0) {
        return "fail";
    }
    char shared_name[250];
    memcpy(shared_name, parts[1], name_len - 6);
    shared_name[name_len - 6] = '\0';
    if (!ValidSharedName(shared_name)) {
        return "fail";
    }

    char *tracker_text;
    const char *read_reply = ReadTrackerCopy(torrent_dir, parts[1], &tracker_text);
    if (strcmp(read_reply, "succ") != 0) {
        return read_reply;
    }
    char md5[33];
    if (GetMD5(tracker_text, strlen(tracker_text), md5) == -1) {
        free(tracker_text);
        return "fail";
    }

    char end_line[64];
    snprintf(end_line, sizeof end_line, "<REP GET END %s>\n", md5);
    if (SendPeerReply(peer_fd, "<REP GET BEGIN>\n") == -1 ||
        SendPeerReply(peer_fd, tracker_text) == -1 ||
        SendPeerReply(peer_fd, end_line) == -1) {
        fprintf(stderr, "Could not send tracker file.\n");
    }
    free(tracker_text);
    return NULL;
}

void *ServeTrackerPeer(void *session_ptr)
{
    /* Take the listener's session copy and release its heap allocation. */
    PeerSession peer = *(PeerSession *)session_ptr;
    free(session_ptr);
    int peer_fd = peer.peer_fd;
    printf("Worker handling socket %d. Tracker directory: %s\n",
           peer_fd, peer.torrent_dir);

    char cmd_buf[TRACKER_CMD_SIZE];

    /* TODO: assemble a complete command here; TCP can split a request. */
    ssize_t n_read = recv(peer_fd, cmd_buf, sizeof cmd_buf - 1, 0);
    if (n_read <= 0) {
        if (n_read < 0) {
            perror("recv");
            SendPeerReply(peer_fd, "fail");
        }
        close(peer_fd);
        return NULL;
    }
    cmd_buf[n_read] = '\0';
    printf("Received: %s\n", cmd_buf);

    char *parts[TRACKER_CMD_FIELDS];
    int n_parts = SplitTrackerCommand(cmd_buf, parts);
    const char *tracker_reply = "fail";
    if (n_parts > 0 && strcmp(parts[0], "createtracker") == 0) {
        tracker_reply = HandleSeedCreate(peer.torrent_dir, parts, n_parts);
    } else if (n_parts > 0 && strcmp(parts[0], "updatetracker") == 0) {
        tracker_reply = HandlePeerUpdate(peer.torrent_dir, parts, n_parts);
    } else if (n_parts > 0 && strcmp(parts[0], "REQ") == 0) {
        tracker_reply = HandleFileList(peer_fd, peer.torrent_dir, parts, n_parts);
    } else if (n_parts > 0 && strcmp(parts[0], "GET") == 0) {
        tracker_reply = HandleTrackerGet(peer_fd, peer.torrent_dir, parts, n_parts);
    }

    /* LIST and GET send their own multiline replies. */
    if (tracker_reply != NULL) {
        printf("Tracker response: %s\n", tracker_reply);
        if (SendPeerReply(peer_fd, tracker_reply) == -1) {
            fprintf(stderr, "Could not send tracker response.\n");
        }
    }
    close(peer_fd);
    return NULL;
}
