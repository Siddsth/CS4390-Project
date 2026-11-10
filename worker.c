/* Expose POSIX functions such as strtok_r() before including headers. */
#define _POSIX_C_SOURCE 200809L
#include "worker.h"
#include "tracker_files.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST_SIZE 1024
#define MAX_FIELDS 7 /* Command name plus up to six arguments. */

/* Send all of the text, even if send() writes only part of it. */
static int SendText(int socket, const char *text)
{
    size_t remaining = strlen(text);
    while (remaining > 0) {
        /* send() returns a signed byte count or -1. MSG_NOSIGNAL lets us
           handle a disconnected peer as a send error without SIGPIPE. */
        ssize_t sent = send(socket, text, remaining, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue; /* Retry after a signal interrupted the call. */
        }
        if (sent <= 0) {
            return -1;
        }
        /* Move the pointer past the bytes already sent and retry the rest. */
        text += sent;
        remaining -= (size_t)sent;
    }
    return 0;
}

/* Read a line from this peer and repeat the question for invalid input. */
static int ConfirmCreate(int socket)
{
    const char *prompt = "Create track file with above information? [Y] Yes [N] No\n";
    while (1) {
        if (SendText(socket, prompt) == -1) {
            return 0;
        }

        /* Keep the buffer small, but read through the newline even if the
           answer is too long so the next attempt starts on a fresh line. */
        char answer[4];
        size_t length = 0;
        int too_long = 0;
        char character;
        while (1) {
            ssize_t received = recv(socket, &character, 1, 0);
            if (received < 0 && errno == EINTR) {
                continue;
            }
            /* Zero means the peer closed its sending side. A negative value
               here means a receive error other than EINTR. */
            if (received <= 0) {
                return 0;
            }
            if (character == '\n') {
                break;
            }
            if (length < sizeof answer - 1) {
                answer[length++] = character;
            } else {
                too_long = 1;
            }
        }
        /* Accept a line ending in either LF or CRLF. */
        if (length > 0 && answer[length - 1] == '\r') {
            length--;
        }
        answer[length] = '\0';
        if (!too_long && length == 1 && answer[0] == 'Y') {
            return 1;
        }
        if (!too_long && length == 1 && answer[0] == 'N') {
            return 0;
        }
    }
}


/* Split the command and its fields. An extra field makes the request invalid. */
static int SplitRequest(char *message, char *fields[MAX_FIELDS])
{
    /* Skip leading whitespace and an optional opening angle bracket. */
    message += strspn(message, " \t\r\n");
    if (message[0] == '<') {
        message++;
    }
    int count = 0;
    /* strtok_r() splits message in place. Each field points into message,
       and next holds the parsing position for this worker's request. */
    char *next;
    char *field = strtok_r(message, " \t\r\n>", &next);
    while (field != NULL) {
        if (count == MAX_FIELDS) {
            return -1;
        }
        fields[count++] = field;
        /* NULL tells strtok_r() to continue from the saved position. */
        field = strtok_r(NULL, " \t\r\n>", &next);
    }
    return count;
}

static int ValidFilename(const char *filename)
{
    /* Require a single filename. The length cap leaves six characters for
       .track within a 255-character filename limit. */
    return strlen(filename) <= 249 && strchr(filename, '/') == NULL &&
           strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0;
}

/* Accept unsigned decimal numbers and reject values too large to represent. */
static int ReadNumber(const char *text, unsigned long long *value)
{
    if (text[0] == '\0' || strlen(text) > 31 ||
        strspn(text, "0123456789") != strlen(text)) {
        return 0;
    }
    /* Store strtoull()'s base-10 result through value. Reset errno so an
       earlier call's error cannot be mistaken for overflow here. */
    errno = 0;
    *value = strtoull(text, NULL, 10);
    return errno != ERANGE;
}

static int ValidPeer(const char *ip, const char *port)
{
    /* inet_pton() checks the IPv4 text. && evaluates left to right, so the
       port range is checked only after ReadNumber() has filled number. */
    struct in_addr address;
    unsigned long long number;
    return inet_pton(AF_INET, ip, &address) == 1 &&
           strlen(port) <= 5 && ReadNumber(port, &number) &&
           number >= 1 && number <= 65535;
}

static const char *HandleCreate(int socket, const char *directory,
                                char *fields[], int count)
{
    if (count != 7) {
        return "fail";
    }
    /* Designated initializers name the struct members. fields[0] holds
       the command, so the actual request arguments start at fields[1]. */
    CreateRequest request = {
        .filename = fields[1],
        .file_size = fields[2],
        .description = fields[3],
        .md5 = fields[4],
        .ip = fields[5],
        .port = fields[6]
    };
    /* Check the size as a number and the MD5 as 32 hexadecimal characters.
       The request keeps the original text for writing to the tracker file. */
    unsigned long long size;
    if (!ValidFilename(request.filename) ||
        !ReadNumber(request.file_size, &size) ||
        strlen(request.description) > 255 ||
        strlen(request.md5) != 32 ||
        strspn(request.md5, "0123456789abcdefABCDEF") != 32 ||
        !ValidPeer(request.ip, request.port)) {
        return "fail";
    }

    char details[REQUEST_SIZE];
    int length = snprintf(details, sizeof details,
        "Filename: %s\nFile size: %s bytes\nDescription: %s\n"
        "MD5: %s\nPeer IP: %s\nPeer port: %s\nSeed range: 0 to %s\n",
        request.filename, request.file_size, request.description,
        request.md5, request.ip, request.port, request.file_size);
    /* snprintf() reports the length it wanted to write, excluding '\0'.
       A result at least as large as the buffer means the text was cut off. */
    if (length < 0 || (size_t)length >= sizeof details) {
        return "fail";
    }
    printf("%s", details);

    /* The file operation starts only after the peer sees the details and
       confirms. If sending fails, || skips the confirmation call. */
    if (SendText(socket, details) == -1 || !ConfirmCreate(socket)) {
        printf("Tracker creation cancelled.\n");
        return NULL;
    }

    /* Keep the short status in the server log and send a readable message
       to the peer for success and duplicate-file errors. */
    const char *result = CreateTracker(directory, &request);
    if (strcmp(result, "succ") == 0) {
        snprintf(details, sizeof details, "Tracker for %s created\n", request.filename);
    } else if (strcmp(result, "ferr") == 0) {
        snprintf(details, sizeof details, "File Error: %s tracker already exists\n",
                 request.filename);
    } else {
        return result;
    }

    printf("Tracker response: %s\n", result);
    if (SendText(socket, details) == -1) {
        fprintf(stderr, "Could not send tracker response.\n");
    }
    return NULL; /* The peer response was handled here. */
}

static const char *HandleUpdate(const char *directory, char *fields[], int count)
{
    if (count != 6) {
        return "fail";
    }
    UpdateRequest request = {
        .filename = fields[1],
        .start = fields[2],
        .end = fields[3],
        .ip = fields[4],
        .port = fields[5]
    };
    /* Convert both offsets before comparing them numerically. */
    unsigned long long start, end;
    if (!ValidFilename(request.filename) ||
        !ReadNumber(request.start, &start) || !ReadNumber(request.end, &end) ||
        start > end || !ValidPeer(request.ip, request.port)) {
        return "fail";
    }
    return UpdateTracker(directory, &request);
}

/* One worker receives one request, handles it, and closes the connection. */
void *Worker(void *argument)
{
    /* pthread_create() passes a void pointer. Cast it back to WorkerArgs *,
       then dereference it to copy the struct before freeing the original. */
    WorkerArgs args = *(WorkerArgs *)argument;
    free(argument);
    int socket = args.connected_socket;
    printf("Worker handling socket %d. Tracker directory: %s\n",
           socket, args.directory);

    char message[REQUEST_SIZE];
    /* Reserve one byte for '\0'. This initial read handles one buffer of
       bytes. TCP can split a request across multiple reads. */
    ssize_t received = recv(socket, message, sizeof message - 1, 0);
    if (received <= 0) {
        if (received < 0) {
            perror("recv");
            SendText(socket, "fail");
        }
        close(socket);
        return NULL;
    }
    message[received] = '\0';
    printf("Received: %s\n", message);

    char *fields[MAX_FIELDS];
    int count = SplitRequest(message, fields);
    const char *reply = "fail";
    if (count > 0 && strcmp(fields[0], "createtracker") == 0) {
        reply = HandleCreate(socket, args.directory, fields, count);
    } else if (count > 0 && strcmp(fields[0], "updatetracker") == 0) {
        reply = HandleUpdate(args.directory, fields, count);
    }

    /* Create handles some responses itself. A NULL reply tells the worker
       it can close the connection without sending any more text. */
    if (reply != NULL) {
        printf("Tracker response: %s\n", reply);
        if (SendText(socket, reply) == -1) {
            fprintf(stderr, "Could not send tracker response.\n");
        }
    }
    close(socket);
    return NULL;
}
