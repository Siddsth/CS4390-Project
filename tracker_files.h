#ifndef TRACKER_FILES_H
#define TRACKER_FILES_H

#include <stddef.h>

/* Fields point into the worker's request buffer and are validated before use. */
typedef struct {
    const char *filename;
    const char *file_size;
    const char *description;
    const char *md5;
    const char *ip;
    const char *port;
} CreateRequest;

typedef struct {
    const char *filename;
    const char *start;
    const char *end;
    const char *ip;
    const char *port;
} UpdateRequest;

typedef struct {
    char filename[250];
    char file_size[32];
    char md5[33];
} TrackerListing;

/* Return the protocol reply: "succ", "ferr", or "fail". */
const char *CreateSeedTracker(const char *torrent_dir, const CreateRequest *seed);
const char *UpdatePeerEntry(const char *torrent_dir, const UpdateRequest *peer_update);

/* Caller frees the returned list. Return 0 on success or -1 on failure. */
int ReadTrackerList(const char *torrent_dir, TrackerListing **files, size_t *file_count);

/* On success, caller frees the returned tracker text. */
const char *ReadTrackerCopy(const char *torrent_dir, const char *track_name, char **text);

#endif

