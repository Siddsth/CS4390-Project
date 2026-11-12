#ifndef PEER_H
#define PEER_H

#include <arpa/inet.h>

#define TRACKER_LINE_SIZE 1024

typedef struct {
    int tracker_port;
    char tracker_ip[INET_ADDRSTRLEN];
    int update_seconds; /* Parsed now; periodic updatetracker is still to be added. */
    int listen_port;
    char shared_folder[256];
} PeerSettings;

/* Return 0 on success or -1 on failure. */
int RunTrackerClient(const PeerSettings *peer);
int DownloadFromSeed(const char *seed_ip, int seed_port, const char *filename);
int StartSeedListener(const PeerSettings *seed);

#endif
