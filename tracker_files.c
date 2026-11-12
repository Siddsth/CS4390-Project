#define _POSIX_C_SOURCE 200809L
#include "tracker_files.h"

#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TRACKER_PATH_SIZE 512

/* This separator is part of the required .track file format. */
#define PEER_LIST_MARKER "#list of peers follows next\n"

/* An update must wait until the seed finishes writing the initial .track file. */
static pthread_mutex_t trackfile_lock = PTHREAD_MUTEX_INITIALIZER;

int ReadTrackerList(const char *torrent_dir, TrackerListing **files, size_t *file_count)
{
    *files = NULL;
    *file_count = 0;
    if (pthread_mutex_lock(&trackfile_lock) != 0) {
        return -1;
    }

    DIR *torrents = opendir(torrent_dir);
    if (torrents == NULL) {
        pthread_mutex_unlock(&trackfile_lock);
        return -1;
    }

    int list_status = 0;
    while (1) {
        errno = 0;
        struct dirent *entry = readdir(torrents);
        if (entry == NULL) {
            if (errno != 0) {
                list_status = -1;
            }
            break;
        }
        size_t name_len = strlen(entry->d_name);
        if (name_len <= 6 || strcmp(entry->d_name + name_len - 6, ".track") != 0) {
            continue;
        }

        char track_path[TRACKER_PATH_SIZE];
        snprintf(track_path, sizeof track_path, "%s/%s", torrent_dir, entry->d_name);
        struct stat track_info;
        if (stat(track_path, &track_info) == -1) {
            list_status = -1;
            break;
        }
        if (!S_ISREG(track_info.st_mode)) {
            continue;
        }

        FILE *trackfile = fopen(track_path, "r");
        if (trackfile == NULL) {
            list_status = -1;
            break;
        }
        TrackerListing shared_file = {0};
        char row[512];
        while (fgets(row, sizeof row, trackfile) != NULL) {
            sscanf(row, "Filename: %249s", shared_file.filename);
            sscanf(row, "Filesize: %31s", shared_file.file_size);
            sscanf(row, "MD5: %32s", shared_file.md5);
            if (shared_file.filename[0] != '\0' && shared_file.file_size[0] != '\0' &&
                shared_file.md5[0] != '\0') {
                break;
            }
        }
        int read_error = ferror(trackfile);
        int close_status = fclose(trackfile);
        if (read_error || close_status != 0 || shared_file.filename[0] == '\0' ||
            shared_file.file_size[0] == '\0' || strlen(shared_file.md5) != 32) {
            list_status = -1;
            break;
        }

        TrackerListing *larger_list = realloc(*files, (*file_count + 1) * sizeof **files);
        if (larger_list == NULL) {
            list_status = -1;
            break;
        }
        *files = larger_list;
        (*files)[*file_count] = shared_file;
        (*file_count)++;
    }

    if (closedir(torrents) == -1) {
        list_status = -1;
    }
    pthread_mutex_unlock(&trackfile_lock);
    if (list_status == -1) {
        free(*files);
        *files = NULL;
        *file_count = 0;
    }
    return list_status;
}

const char *ReadTrackerCopy(const char *torrent_dir, const char *track_name, char **text)
{
    *text = NULL;
    char track_path[TRACKER_PATH_SIZE];
    int path_len = snprintf(track_path, sizeof track_path, "%s/%s", torrent_dir, track_name);
    if (path_len < 0 || (size_t)path_len >= sizeof track_path ||
        pthread_mutex_lock(&trackfile_lock) != 0) {
        return "fail";
    }

    FILE *trackfile = fopen(track_path, "rb");
    if (trackfile == NULL) {
        const char *reply = errno == ENOENT ? "ferr" : "fail";
        pthread_mutex_unlock(&trackfile_lock);
        return reply;
    }

    long file_bytes = -1;
    if (fseek(trackfile, 0, SEEK_END) == 0) {
        file_bytes = ftell(trackfile);
    }
    if (file_bytes <= 0 || fseek(trackfile, 0, SEEK_SET) != 0) {
        fclose(trackfile);
        pthread_mutex_unlock(&trackfile_lock);
        return "fail";
    }

    char *copy = malloc((size_t)file_bytes + 1);
    if (copy == NULL) {
        fclose(trackfile);
        pthread_mutex_unlock(&trackfile_lock);
        return "fail";
    }
    size_t n_read = fread(copy, 1, (size_t)file_bytes, trackfile);
    int close_status = fclose(trackfile);
    pthread_mutex_unlock(&trackfile_lock);
    if (n_read != (size_t)file_bytes || close_status != 0 ||
        copy[n_read - 1] != '\n' || memchr(copy, '\0', n_read) != NULL) {
        free(copy);
        return "fail";
    }
    copy[n_read] = '\0';
    *text = copy;
    return "succ";
}

const char *CreateSeedTracker(const char *torrent_dir, const CreateRequest *seed)
{
    char track_path[TRACKER_PATH_SIZE];
    int path_len = snprintf(track_path, sizeof track_path, "%s/%s.track",
                            torrent_dir, seed->filename);

    if (path_len < 0 || (size_t)path_len >= sizeof track_path) {
        return "fail";
    }

    time_t last_seen = time(NULL);
    if (last_seen == (time_t)-1) {
        return "fail";
    }

    if (pthread_mutex_lock(&trackfile_lock) != 0) {
        return "fail";
    }

    /* Another seed may advertise the same filename; keep the first tracker. */
    FILE *trackfile = fopen(track_path, "wx");
    if (trackfile == NULL) {
        const char *reply = "fail";
        if (errno == EEXIST) {
            reply = "ferr";
        }

        pthread_mutex_unlock(&trackfile_lock);
        return reply;
    }

    /* The seed already holds the whole file, so it supplies the first peer row. */
    int written = fprintf(trackfile,
        "Filename: %s\n"
        "Filesize: %s\n"
        "Description: %s\n"
        "MD5: %s\n"
        PEER_LIST_MARKER
        "%s:%s:0:%s:%lld\n",
        seed->filename, seed->file_size, seed->description,
        seed->md5, seed->ip, seed->port, seed->file_size,
        (long long)last_seen);

    /* Buffered writes can fail at close; an incomplete tracker must be removed. */
    int close_status = fclose(trackfile);
    const char *reply = "succ";
    if (written < 0 || close_status != 0) {
        unlink(track_path);
        reply = "fail";
    }

    pthread_mutex_unlock(&trackfile_lock);
    return reply;
}

/* Refresh one peer's range and last-seen time without changing file metadata. */
static int RewritePeerList(FILE *trackfile, FILE *replacement,
                          const UpdateRequest *peer_update, time_t last_seen)
{
    char *row = NULL;
    size_t row_capacity = 0;
    int in_peer_list = 0;
    int peer_found = 0;
    int io_error = 0;

    while (getline(&row, &row_capacity, trackfile) != -1) {
        if (strcmp(row, PEER_LIST_MARKER) == 0) {
            in_peer_list = 1;
        } else if (in_peer_list) {
            char listed_ip[16];
            unsigned int listed_port;

            /* Peers can share an IP, so match the listening port as well. */
            if (sscanf(row, "%15[^:]:%u:", listed_ip, &listed_port) == 2 &&
                strcmp(listed_ip, peer_update->ip) == 0 &&
                listed_port == strtoul(peer_update->port, NULL, 10)) {
                if (!peer_found && fprintf(replacement, "%s:%s:%s:%s:%lld\n",
                        peer_update->ip, peer_update->port, peer_update->start,
                        peer_update->end, (long long)last_seen) < 0) {
                    io_error = 1;
                }

                /* Keep just one row if this peer was listed more than once. */
                peer_found = 1;
                continue;
            }
        }
        if (fputs(row, replacement) == EOF) {
            io_error = 1;
        }
    }

    if (ferror(trackfile) || !in_peer_list) {
        io_error = 1;
    }
    if (!io_error && !peer_found && fprintf(replacement, "%s:%s:%s:%s:%lld\n",
            peer_update->ip, peer_update->port, peer_update->start, peer_update->end,
            (long long)last_seen) < 0) {
        io_error = 1;
    }
    free(row);
    return io_error ? -1 : 0;
}

const char *UpdatePeerEntry(const char *torrent_dir, const UpdateRequest *peer_update)
{
    char track_path[TRACKER_PATH_SIZE];
    int path_len = snprintf(track_path, sizeof track_path, "%s/%s.track",
                            torrent_dir, peer_update->filename);
    if (path_len < 0 || (size_t)path_len >= sizeof track_path) {
        return "fail";
    }

    /* Put the replacement beside the tracker so rename can replace it atomically. */
    char tmp_path[TRACKER_PATH_SIZE + 16];
    int tmp_path_len = snprintf(tmp_path, sizeof tmp_path,
                                "%s.tmpXXXXXX", track_path);
    if (tmp_path_len < 0 ||
        (size_t)tmp_path_len >= sizeof tmp_path) {
        return "fail";
    }

    time_t last_seen = time(NULL);
    if (last_seen == (time_t)-1) {
        return "fail";
    }

    if (pthread_mutex_lock(&trackfile_lock) != 0) {
        return "fail";
    }

    FILE *trackfile = fopen(track_path, "r");
    if (trackfile == NULL) {
        const char *reply = "fail";
        if (errno == ENOENT) {
            reply = "ferr";
        }

        pthread_mutex_unlock(&trackfile_lock);
        return reply;
    }

    /* Keep the old peer list intact until the replacement is ready. */
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd == -1) {
        fclose(trackfile);
        pthread_mutex_unlock(&trackfile_lock);
        return "fail";
    }

    /* Once fdopen succeeds, fclose(replacement) also closes tmp_fd. */
    FILE *replacement = fdopen(tmp_fd, "w");
    if (replacement == NULL) {
        close(tmp_fd);
        fclose(trackfile);
        unlink(tmp_path);
        pthread_mutex_unlock(&trackfile_lock);
        return "fail";
    }

    int peerlist_status = RewritePeerList(trackfile, replacement, peer_update,
                                         last_seen);

    /* Publish the peer list only after copying and flushing have succeeded. */
    int track_closed = fclose(trackfile);
    int replacement_closed = fclose(replacement);
    if (peerlist_status != 0 || track_closed != 0 ||
        replacement_closed != 0) {
        unlink(tmp_path);
        pthread_mutex_unlock(&trackfile_lock);
        return "fail";
    }

    int rename_status = rename(tmp_path, track_path);
    const char *reply = "succ";
    if (rename_status != 0) {
        unlink(tmp_path);
        reply = "fail";
    }

    pthread_mutex_unlock(&trackfile_lock);
    return reply;
}

