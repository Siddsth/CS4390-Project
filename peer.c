#define _POSIX_C_SOURCE 200809L
#include "peer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int LoadPeerSettings(PeerSettings *settings)
{
    FILE *client_cfg = fopen("clientThreadConfig.cfg", "r");
    if (client_cfg == NULL) {
        perror("clientThreadConfig.cfg");
        return -1;
    }
    char trailing;

    int fields_read = fscanf(client_cfg, "%d %15s %d %c", &settings->tracker_port,
                             settings->tracker_ip, &settings->update_seconds, &trailing);
    fclose(client_cfg);

    if (fields_read != 3 || settings->tracker_port < 1 ||
        settings->tracker_port > 65535 || settings->update_seconds < 1) {
        fprintf(stderr, "clientThreadConfig.cfg needs a tracker port, IP, and update interval in seconds.\n");
        return -1;
    }

    /* The course gives each peer separate tracker-client and serving settings. */
    FILE *server_cfg = fopen("serverThreadConfig.cfg", "r");
    if (server_cfg == NULL) {
        perror("serverThreadConfig.cfg");
        return -1;
    }

    fields_read = fscanf(server_cfg, "%d %255s %c", &settings->listen_port,
                         settings->shared_folder, &trailing);
    fclose(server_cfg);
    if (fields_read != 2 || settings->listen_port < 1 || settings->listen_port > 65535) {
        fprintf(stderr, "serverThreadConfig.cfg needs a peer port and shared folder.\n");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    /* Direct seed requests can be tested before tracker GET supplies the endpoint. */
    if (argc != 1) {
        if (argc != 5 || strcmp(argv[1], "--request-seed") != 0) {
            fprintf(stderr, "Usage: %s [--request-seed IP PORT FILENAME]\n", argv[0]);
            return 1;
        }

        char *port_end;
        errno = 0;
        long seed_port = strtol(argv[3], &port_end, 10);
        if (errno == ERANGE || *port_end != '\0' ||
            seed_port < 1 || seed_port > 65535) {
            fprintf(stderr, "The seed port must be a number from 1 to 65535.\n");
            return 1;
        }

        return DownloadFromSeed(argv[2], (int)seed_port, argv[4]) == 0 ? 0 : 1;
    }

    PeerSettings peer;
    if (LoadPeerSettings(&peer) == -1) {
        return 1;
    }
    if (mkdir(peer.shared_folder, 0777) == -1 && errno != EEXIST) {
        perror(peer.shared_folder);
        return 1;
    }
    if (StartSeedListener(&peer) == -1) {
        return 1;
    }
    return RunTrackerClient(&peer) == 0 ? 0 : 1;
}
