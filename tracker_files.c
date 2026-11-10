/* Expose POSIX file functions such as getline() and mkstemp(). */
#define _POSIX_C_SOURCE 200809L
#include "tracker_files.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TRACKER_PATH_SIZE 512
/* The marker separates file metadata from the list of peer rows. */
#define PEER_HEADER "#list of peers follows next\n"

/* Every worker uses this mutex. Holding it for the whole read/write operation
   keeps two workers from overwriting each other's updates. */
static pthread_mutex_t tracker_lock = PTHREAD_MUTEX_INITIALIZER;

/* Return a protocol status string that the worker can send or translate. */
const char *CreateTracker(const char *directory, const CreateRequest *request)
{
    char path[TRACKER_PATH_SIZE];
    int length = snprintf(path, sizeof path, "%s/%s.track",
                          directory, request->filename);
    /* snprintf() reports how many characters it needed, excluding '\0'.
       Reject a path that would not fit in the buffer. */
    if (length < 0 || (size_t)length >= sizeof path) {
        return "fail";
    }
    if (pthread_mutex_lock(&tracker_lock) != 0) {
        return "fail";
    }

    /* The x in "wx" makes creation exclusive. If the file exists, fopen()
       fails with EEXIST and the worker can report ferr. */
    FILE *file = fopen(path, "wx");
    if (file == NULL) {
        const char *reply = errno == EEXIST ? "ferr" : "fail";
        pthread_mutex_unlock(&tracker_lock);
        return reply;
    }

    /* time(NULL) returns the current timestamp or (time_t)-1 on failure. */
    time_t timestamp = time(NULL);
    int written = -1;
    if (timestamp != (time_t)-1) {
        /* Adjacent string literals combine into a single format string.
           The first peer row records IP:port:start:end:timestamp. */
        written = fprintf(file,
            "Filename: %s\nFilesize: %s\nDescription: %s\nMD5: %s\n"
            PEER_HEADER "%s:%s:0:%s:%lld\n",
            request->filename, request->file_size, request->description,
            request->md5, request->ip, request->port, request->file_size,
            (long long)timestamp);
    }

    /* fclose() flushes buffered output. If either writing or flushing fails,
       remove the incomplete file so a later creation can start fresh. */
    int closed = fclose(file);
    const char *reply = "succ";
    if (written < 0 || closed != 0) {
        unlink(path);
        reply = "fail";
    }
    pthread_mutex_unlock(&tracker_lock);
    return reply;
}

/* Copy the metadata and other peers; replace this peer or append a new entry. */
static int CopyWithUpdate(FILE *original, FILE *updated,
                          const UpdateRequest *request, time_t timestamp)
{
    /* getline() allocates and grows line as needed. Pass its address so
       getline() can change the pointer, and free the final buffer below. */
    char *line = NULL;
    size_t capacity = 0;
    int in_peers = 0;
    int found = 0;
    int error = 0;

    while (getline(&line, &capacity, original) != -1) {
        if (strcmp(line, PEER_HEADER) == 0) {
            in_peers = 1;
        } else if (in_peers) {
            char saved_ip[16];
            unsigned int saved_port;
            /* %15[^:] reads up to 15 characters before a colon. Match both
               the IP text and the numeric port to identify the peer. */
            if (sscanf(line, "%15[^:]:%u:", saved_ip, &saved_port) == 2 &&
                strcmp(saved_ip, request->ip) == 0 &&
                saved_port == strtoul(request->port, NULL, 10)) {
                if (!found && fprintf(updated, "%s:%s:%s:%s:%lld\n",
                        request->ip, request->port, request->start,
                        request->end, (long long)timestamp) < 0) {
                    error = 1;
                }
                /* Skip the old row. If the same peer appears again, skip
                   that duplicate too, since we already wrote its update. */
                found = 1;
                continue;
            }
        }
        if (fputs(line, updated) == EOF) {
            error = 1;
        }
    }

    /* A read error or a missing peer-list marker means this copy cannot be used. */
    if (ferror(original) || !in_peers) {
        error = 1;
    }
    if (!error && !found && fprintf(updated, "%s:%s:%s:%s:%lld\n",
            request->ip, request->port, request->start, request->end,
            (long long)timestamp) < 0) {
        error = 1;
    }
    free(line);
    return error ? -1 : 0;
}

const char *UpdateTracker(const char *directory, const UpdateRequest *request)
{
    char path[TRACKER_PATH_SIZE];
    char temp_path[TRACKER_PATH_SIZE + 16];
    int length = snprintf(path, sizeof path, "%s/%s.track",
                          directory, request->filename);
    if (length < 0 || (size_t)length >= sizeof path) {
        return "fail";
    }
    /* mkstemp() replaces the trailing Xs with a unique suffix in the same
       directory as the tracker, ready for rename() after the write. */
    snprintf(temp_path, sizeof temp_path, "%s.tmpXXXXXX", path);
    if (pthread_mutex_lock(&tracker_lock) != 0) {
        return "fail";
    }

    /* Default to fail. If the original file is missing, return ferr. */
    const char *reply = "fail";
    FILE *original = fopen(path, "r");
    FILE *updated = NULL;
    int temp_created = 0;
    if (original == NULL) {
        reply = errno == ENOENT ? "ferr" : "fail";
        goto cleanup;
    }

    /* Write a separate copy so an unsuccessful update leaves the original intact. */
    int fd = mkstemp(temp_path);
    if (fd == -1) {
        goto cleanup;
    }
    temp_created = 1;
    /* Turn the open descriptor into a stream for fprintf() and fputs().
       Once fdopen() succeeds, fclose(updated) closes the descriptor too. */
    updated = fdopen(fd, "w");
    if (updated == NULL) {
        close(fd);
        goto cleanup;
    }

    time_t timestamp = time(NULL);
    if (timestamp == (time_t)-1 ||
        CopyWithUpdate(original, updated, request, timestamp) == -1) {
        goto cleanup;
    }

    /* Close both streams before publishing the replacement. Set the pointers
       to NULL so cleanup does not close the same streams twice. */
    int read_closed = fclose(original);
    original = NULL;
    int write_closed = fclose(updated);
    updated = NULL;
    if (read_closed == 0 && write_closed == 0 &&
        rename(temp_path, path) == 0) {
        temp_created = 0;
        reply = "succ";
    }

/* Error paths jump here to close any open streams, remove an unused
   temporary file, and always release the mutex. */
cleanup:
    if (original != NULL) {
        fclose(original);
    }
    if (updated != NULL) {
        fclose(updated);
    }
    if (temp_created) {
        unlink(temp_path);
    }
    pthread_mutex_unlock(&tracker_lock);
    return reply;
}

