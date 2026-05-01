#include "helper.h"

// Socket creation wrapper
// Returns a socket descriptor or exits on failure
int Socket(int family, int type, int protocol) {
    int n;
    if ((n = socket(family, type, protocol)) < 0) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }
    return n;
}

// Bind wrapper
// Binds the socket to an address or exits on failure
void Bind(int fd, const struct sockaddr *sa, socklen_t salen) {
    if (bind(fd, sa, salen) < 0) {
        perror("bind error");
        exit(EXIT_FAILURE);
    }
}

// Connect wrapper
// Connects to a remote server or exits on failure
void Connect(int fd, const struct sockaddr *sa, socklen_t salen) {
    if (connect(fd, sa, salen) < 0) {
        perror("connect error");
        exit(EXIT_FAILURE);
    }
}

// Listen wrapper
// Transitions the socket to listening state or exits on failure
void Listen(int fd, int backlog) {
    if (listen(fd, backlog) < 0) {
        perror("listen error");
        exit(EXIT_FAILURE);
    }
}

// Accept wrapper
// Accepts a new connection or exits on failure
int Accept(int fd, struct sockaddr *sa, socklen_t *salenptr) {
    int n;
    if ((n = accept(fd, sa, salenptr)) < 0) {
        perror("accept error");
        exit(EXIT_FAILURE);
    }
    return n;
}

// Close wrapper
// Closes the file/socket descriptor
void Close(int fd) {
    if (close(fd) < 0) {
        perror("close error");
        exit(EXIT_FAILURE);
    }
}

// Send helper function
ssize_t Send(int fd, const void *ptr, size_t nbytes, int flags) {
    ssize_t n;
    if ((n = send(fd, ptr, nbytes, flags)) < 0) {
        perror("send error");
        exit(EXIT_FAILURE);
    }
    return n;
}
// Make a socket non-blocking
// Returns 0 on success, -1 on failure
int make_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return -1;
    }
    return 0;
}
// Create a listening socket on the specified port
// Returns the server socket descriptor or exits on failure
int make_listener(int port) {
    int server_fd = Socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    // Allow reuse of the port immediately after the server is restarted
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Set up version 4 address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    // Bind and listen
    Bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    Listen(server_fd, BACKLOG);
    return server_fd;
}
int recv_all(int fd, void *buf, size_t len)
{
    size_t total = 0;
    char  *ptr   = (char *)buf;
    while (total < len) {
        ssize_t r = recv(fd, ptr + total, len - total, 0);
        if (r == 0) { printf("[INFO] Client disconnected.\n"); return -1; }
        if (r<0){
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more data available right now, try again later
                continue;
            }
            perror("recv error");
            return -1;
        }
        
        total += (size_t)r;
    }
    return 0;
}
int recv_u32(int fd, uint32_t *out)
{
    uint32_t net;
    if (recv_all(fd, &net, 4) < 0) return -1;
    *out = ntohl(net);
    return 0;
}
int send_response(int fd, uint8_t status, const char *data, uint32_t data_len)
{
    uint32_t len_net = htonl(data_len);
    if (send_all(fd, &status, 1) < 0) return -1;
    if (send_all(fd, &len_net, 4) < 0) return -1;
    if (data_len > 0)
        if (send_all(fd, data, data_len) < 0) return -1;
    return 0;
}
int send_all(int fd, const void *buf, size_t len)
{
    size_t      total = 0;
    const char *ptr   = (const char *)buf;
    while (total < len) {
        ssize_t s = send(fd, ptr + total, len - total, 0);
        if (s <= 0) {
            perror("send error");
            return -1;
        }

        total += (size_t)s;
    }
    return 0;
}
int recv_all_file(int fd, const char *save_path, uint32_t file_size)
{
    FILE *f = fopen(save_path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot create file '%s': %s\n", save_path, strerror(errno));
        return -1;
    }

    size_t total_received = 0;
    char   chunk[CHUNK_SIZE];

    while (total_received < file_size) {
        size_t  want = file_size - total_received;
        if (want > CHUNK_SIZE) want = CHUNK_SIZE;

        ssize_t r = recv(fd, chunk, want, 0);
        if (r <= 0) {
            fclose(f);
            return -1;
        }

        fwrite(chunk, 1, (size_t)r, f);
        total_received += (size_t)r;
    }

    fclose(f);
    return 0;
}