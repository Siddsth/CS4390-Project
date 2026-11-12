#ifndef WORKER_H
#define WORKER_H

/* One accepted tracker connection; its worker takes ownership of this session. */
typedef struct {
    int peer_fd;
    char torrent_dir[10];
} PeerSession;

void *ServeTrackerPeer(void *session_ptr);

#endif
