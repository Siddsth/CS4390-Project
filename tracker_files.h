#ifndef TRACKER_FILES_H
#define TRACKER_FILES_H

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

/* Return the protocol reply: "succ", "ferr", or "fail". */
const char *CreateTracker(const char *directory, const CreateRequest *request);
const char *UpdateTracker(const char *directory, const UpdateRequest *request);

#endif

