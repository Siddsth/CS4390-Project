/* Request POSIX declarations such as fdopen() before including any headers. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST_SIZE 1024
/* Match the tracker's prompt exactly, including its trailing newline. */
#define CREATE_PROMPT "Create track file with above information? [Y] Yes [N] No\n"

typedef struct {
    int tracker_port;
    char tracker_ip[INET_ADDRSTRLEN]; /* Enough room for IPv4 text and its '\0'. */
    int update_interval;             /* Read in seconds for future periodic updates. */
    int peer_port;
    char shared_folder[256];
} PeerConfig;

/* static keeps this helper private to peer.c. The pointer lets it fill main's struct. */
static int ReadConfig(PeerConfig *config)
{
    FILE *client = fopen("clientThreadConfig.cfg", "r");
    if (client == NULL) {
        perror("clientThreadConfig.cfg");
        return -1;
    }
    char extra;
    /* -> selects a member through a pointer. & gives fscanf() the address
       of a number to write. An array like tracker_ip already supplies an
       address here. %15s leaves room for '\0', and the final " %c" checks
       for an extra non-whitespace character after the expected fields. */
    int fields = fscanf(client, "%d %15s %d %c", &config->tracker_port,
                        config->tracker_ip, &config->update_interval, &extra);
    fclose(client);
    /* fscanf() counts successful assignments. || stops at the first true
       condition, so a short read never leads us to check an unread field. */
    if (fields != 3 || config->tracker_port < 1 ||
        config->tracker_port > 65535 || config->update_interval < 1) {
        fprintf(stderr, "clientThreadConfig.cfg needs a tracker port, IP, and update interval in seconds.\n");
        return -1;
    }

    FILE *server = fopen("serverThreadConfig.cfg", "r");
    if (server == NULL) {
        perror("serverThreadConfig.cfg");
        return -1;
    }
    /* %255s reads a single folder name without spaces and leaves room for '\0'. */
    fields = fscanf(server, "%d %255s %c", &config->peer_port,
                    config->shared_folder, &extra);
    fclose(server);
    if (fields != 2 || config->peer_port < 1 || config->peer_port > 65535) {
        fprintf(stderr, "serverThreadConfig.cfg needs a peer port and shared folder.\n");
        return -1;
    }
    return 0;
}

/* const lets this function read the config without changing it. */
static int ConnectToTracker(const PeerConfig *config)
{
    /* {0} initializes the entire address structure before we fill its fields. */
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    /* htons() puts the port in network byte order. inet_pton() turns text
       such as "127.0.0.1" into the binary address stored in sin_addr. */
    address.sin_port = htons(config->tracker_port);
    if (inet_pton(AF_INET, config->tracker_ip, &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid tracker IPv4 address in clientThreadConfig.cfg.\n");
        return -1;
    }

    /* An IPv4 stream socket with protocol 0 selects the default, TCP. */
    int tracker_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_socket == -1) {
        perror("socket");
        return -1;
    }
    /* connect() accepts a generic address pointer. The cast gives it that
       pointer type, and sizeof still gives it the actual IPv4 structure size. */
    if (connect(tracker_socket, (struct sockaddr *)&address, sizeof address) == -1) {
        perror("connect");
        close(tracker_socket);
        return -1;
    }
    return tracker_socket;
}

static int SendText(int tracker_socket, const char *text)
{
    /* strlen() excludes the C string terminator. TCP carries the text bytes,
       including the newline, without the terminating '\0'. */
    size_t remaining = strlen(text);
    while (remaining > 0) {
        /* send() can write fewer bytes than requested. Its signed result can
           also hold -1. MSG_NOSIGNAL lets us handle a closed socket as an error
           instead of letting SIGPIPE terminate the process. */
        ssize_t sent = send(tracker_socket, text, remaining, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue; /* Retry when a signal interrupted the call before it sent data. */
        }
        if (sent <= 0) {
            perror("send");
            return -1;
        }
        /* Advance to the first unsent byte. The positive count can now be
           converted to size_t, the same unsigned type as remaining. */
        text += sent;
        remaining -= (size_t)sent;
    }
    return 0;
}

/* TCP can deliver a reply in pieces. fgets() assembles a line from the stream
   and returns any final text when the tracker closes the connection. */
static int ReadReply(FILE *tracker, int tracker_socket)
{
    char line[REQUEST_SIZE];
    int needs_newline = 0;
    while (fgets(line, sizeof line, tracker) != NULL) {
        fputs(line, stdout);
        fflush(stdout);
        /* Short status replies can end without a newline. Keep track of
           whether the terminal needs one after the final reply. */
        needs_newline = strchr(line, '\n') == NULL;
        /* strcmp() returns 0 for an exact match. Only the confirmation prompt
           requires a second message from this peer on the same connection. */
        if (strcmp(line, CREATE_PROMPT) == 0) {
            char answer[REQUEST_SIZE];
            if (fgets(answer, sizeof answer, stdin) == NULL ||
                strchr(answer, '\n') == NULL || SendText(tracker_socket, answer) == -1) {
                return -1;
            }
        }
    }
    /* A NULL result from fgets() could mean either end of input or a read error. */
    if (ferror(tracker)) {
        perror("read tracker reply");
        return -1;
    }
    if (needs_newline) {
        putchar('\n');
    }
    return 0;
}

int main(void)
{
    PeerConfig config;
    if (ReadConfig(&config) == -1) {
        return 1;
    }
    printf("Tracker: %s:%d\n", config.tracker_ip, config.tracker_port);
    printf("Configured peer port: %d. Shared folder: %s\n",
           config.peer_port, config.shared_folder);
    printf("Enter a tracker request: ");
    /* The prompt has no newline, so flush it before waiting for input. */
    fflush(stdout);

    char request[REQUEST_SIZE];
    if (fgets(request, sizeof request, stdin) == NULL) {
        return 1;
    }
    /* fgets() retains the newline if it fits. Require it before sending
       so a line cut short by the buffer is never sent as a complete request. */
    if (strchr(request, '\n') == NULL) {
        fprintf(stderr, "Enter a complete request followed by Enter.\n");
        return 1;
    }

    int tracker_socket = ConnectToTracker(&config);
    if (tracker_socket == -1) {
        return 1;
    }
    /* fdopen() wraps the existing socket in a stream for reading with fgets().
       The same socket descriptor is used by send() for outgoing text. */
    FILE *tracker = fdopen(tracker_socket, "r");
    if (tracker == NULL) {
        perror("fdopen");
        close(tracker_socket);
        return 1;
    }

    int status = SendText(tracker_socket, request);
    if (status == 0) {
        status = ReadReply(tracker, tracker_socket);
    }
    fclose(tracker); /* Closing the stream also closes its socket descriptor. */
    return status == 0 ? 0 : 1;
}
