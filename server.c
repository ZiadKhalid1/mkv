#include "helper.h"
#include "ht.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/*POSIX LIBRARIES USED*/
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>

/*--------------------------------------------------------*//* will point to our hash table instance */

#define DEFAULT_PORT   6379
#define BACKLOG SOMAXCONN
#define MAX_CLIENTS 10000 
/* Command type bytes sent on the wire */
#define CMD_SET        0x01
#define CMD_SETFILE    0x02
#define CMD_GET        0x03
#define CMD_GETFILE    0x04
#define CMD_DEL        0x05
#define CMD_EXPIRE     0x06
#define CMD_SEARCH     0x07
/* Response status bytes */
#define STATUS_OK      0x00
#define STATUS_ERR     0xFF
/* ANSII color codes for pretty terminal output */
#define COL_RESET    
#define COL_RED     
#define COL_GREEN   
#define COL_YELLOW  
#define COL_CYAN    
#define COL_BOLD 
/* ------------------------------------------------------------------ */
              
#define MAX_KEY_LEN    256
#define MAX_VAL_LEN    4096
#define MAX_PATH_LEN   512
/* ------------------------------------------------------------------ */
#ifndef FILE_STORE_DIR
#define FILE_STORE_DIR "store/files"
#endif

ht *global_table = NULL;

/*requestes struct */ 
typedef struct {
    uint8_t  type;
    char    *key;
    uint32_t key_len;

    char    *value;      /* SET string or SETFILE bytes */
    uint32_t val_len;

    char    *path;       /* optional: GETFILE save path (if you keep this design) */
    uint32_t path_len;

    uint32_t ttl;        /* EXPIRE seconds */
} Request;
/* ------------------------------------------------------------------ */


/*functions prototype*/
static  int parse_request(int client_fd, Request *req);
static void free_request(Request *req);
static int read_bytes_or_fail(int fd, char *buf, size_t len);
static void epoll_loop(int server_fd);
static int handle_client(int client_fd);
static void cleanup(void);
static  uint64_t fnv1a64(const char *s);
static int ensure_dir(const char *dir);
static int ensure_store_dirs(void);
static void build_file_path(char out[], size_t out_sz, const char *key);
static void delete_if_file_value(char *old);
static int store_set_string(const char *key, const char *value);
static int store_set_file(int client_fd, const char *key, uint32_t file_size);
static int store_get_string(const char *key, const char **out_val);
static int store_get_file(const char *key, char **out_buf, uint32_t *out_len);


/*--------------------------------------------------------------------------*/
// distroy the hash table and free memory when server shuts down
/*--------------------------------------------------------------------------*/
void cleanup() {
    hti it = ht_iterator(global_table);
    while (ht_next(&it)) {
        free(it.value); // Free the value string
    }
    ht_destroy(global_table); // Free the hash table itself
}

/* FNV-1a 64-bit hash for safe stable filenames */
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Ensure a single directory exists (not recursive) */
static int ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(dir, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* Ensure store/files exists (create parent then child) */
static int ensure_store_dirs(void) {
    if (ensure_dir("store") != 0) return -1;
    if (ensure_dir(FILE_STORE_DIR) != 0) return -1;
    return 0;
}

/* Build final path: store/files/<hash>.bin */
static void build_file_path(char out[], size_t out_sz, const char *key) {
    uint64_t h = fnv1a64(key);
    snprintf(out, out_sz, "%s/%016llx.bin", FILE_STORE_DIR,(unsigned long long)h);
}

/* If old value is file:<path>, delete file */
static void delete_if_file_value(char *old) {
    if (!old) return;
    if (strncmp(old, "file:", 5) == 0) {
        remove(old + 5);
    }
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;

    global_table = ht_create();
    if (!global_table) {
        fprintf(stderr, "Failed to create hash table\n");
        return EXIT_FAILURE;
    }

    if (ensure_store_dirs() != 0) {
        perror("ensure_store_dirs");
        return EXIT_FAILURE;
    }

    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 1024 && p < 49151) port = p;
        else printf("Using default port %d\n", DEFAULT_PORT);
    } else {
        printf("Using default port %d\n", DEFAULT_PORT);
    }

    int server_fd = make_listener(port);
    printf("Server is listening on port %d...\n", port);

    make_non_blocking(server_fd);

    epoll_loop(server_fd);

    cleanup();
    return 0;
}










/* ------------------------------------------------------------------ */
// functions  implementations 
/* ------------------------------------------------------------------ */

// Epoll loop implementation
void epoll_loop(int server_fd)
{
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return;
    }

    struct epoll_event ev, events[MAX_CLIENTS];
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;              // level-triggered 
    ev.data.fd = server_fd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl ADD server_fd");
        Close(epfd);
        return;
    }

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_CLIENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue; // interrupted by signal
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // Accept all pending connections (server_fd is non-blocking)
                while (1) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // no more pending clients right now
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    if (make_non_blocking(client_fd) < 0) {
                        close(client_fd);
                        continue;
                    }

                    memset(&ev, 0, sizeof(ev));
                    ev.events = EPOLLIN;   // keep level-triggered
                    ev.data.fd = client_fd;

                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        perror("epoll_ctl ADD client_fd");
                        close(client_fd);
                        continue;
                    }

                    printf("New client connected: fd %d\n", client_fd);
                }
            } 
            else
            {
                // client socket readable or hangup/error
                if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }

                // handle one request (or more if your handler loops internally)
                // Make handle_client return: 0 keep-open, -1 close
                int rc = handle_client(fd);

                if (rc < 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                }
            }
        }
    }

    Close(epfd);
}
// store a string value in the hash table under the given key

static int store_set_string(const char *key, const char *value) {
    if (!key || !value) return -1;

    /* Prefix "str:" */
    size_t vlen = strlen(value);
    char *entry = (char *)malloc(vlen + 5); /* "str:" + value + '\0' */
    if (!entry) return -1;

    memcpy(entry, "str:", 4);
    memcpy(entry + 4, value, vlen + 1);

    char *old = (char *)ht_get(global_table, key);
    if (old) {
        // If old value is a file, delete the file
        delete_if_file_value(old);
        free(old);
    }

    if (!ht_set(global_table, key, entry)) {
        free(entry);
        return -1;
    }
    return 0;
}
// store a file's contents in the hash table under the given key
static int store_set_file(int client_fd, const char *key, uint32_t file_size) {
    if (!key) return -1;

    char filepath[MAX_PATH_LEN];
    char tmpfile[MAX_PATH_LEN];
    uint64_t h = fnv1a64(key);

    snprintf(filepath, sizeof(filepath), "%s/%016llx.bin", FILE_STORE_DIR, (unsigned long long)h);
    snprintf(tmpfile, sizeof(tmpfile), "%s/%016llx.tmp", FILE_STORE_DIR, (unsigned long long)h);

    FILE *f = fopen(tmpfile, "wb");
    if (!f) { perror("fopen store_set_file"); return -1; }

    size_t total_received = 0;
    char chunk[4096]; // CHUNK_SIZE

    /* Read the file in chunks from the socket directly to disk */
    while (total_received < file_size) {
        size_t want = file_size - total_received;
        if (want > sizeof(chunk)) want = sizeof(chunk);

        ssize_t r = recv(client_fd, chunk, want, 0);
        if (r < 0) {
            // Handle non-blocking socket busy wait
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; 
            fclose(f);
            remove(tmpfile);
            return -1;
        } else if (r == 0) {
            fclose(f);
            remove(tmpfile);
            return -1; // Client disconnected
        }

        fwrite(chunk, 1, (size_t)r, f);
        total_received += (size_t)r;
    }

    if (fclose(f) != 0) { remove(tmpfile); return -1; }

    /* Atomically rename temp file to final path */
    if (rename(tmpfile, filepath) != 0) {
        remove(tmpfile);
        perror("rename store_set_file");
        return -1;
    }

    /* Add entry to hash table */
    size_t plen = strlen(filepath);
    char *entry = (char *)malloc(plen + 6);
    if (!entry) { remove(filepath); return -1; }
    
    memcpy(entry, "file:", 5);
    memcpy(entry + 5, filepath, plen + 1);

    char *old = (char *)ht_get(global_table, key);
    if (old) {
        if (strncmp(old, "file:", 5) == 0) remove(old + 5);
        free(old);
    }

    if (!ht_set(global_table, key, entry)) {
        free(entry);
        remove(filepath);
        return -1;
    }

    return 0;
}
// retrieve a string value from the hash table under the given key
static int store_get_string(const char *key, const char **out_val) {
    char *entry = (char *)ht_get(global_table, key);
    if (!entry) return -1;

    if (strncmp(entry, "str:", 4) != 0) return -1; /* it's a file */

    *out_val = entry + 4; /* skip "str:" */
    return 0;
}
// retrieve a file's contents from the hash table under the given key
static int store_get_file(const char *key, char **out_buf, uint32_t *out_len) {
    char *entry = (char *)ht_get(global_table, key);
    if (!entry) return -1;

    if (strncmp(entry, "file:", 5) != 0) return -1; /* it's a string */

    const char *filepath = entry + 5;

    struct stat st;
    if (stat(filepath, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    if (st.st_size < 0 || st.st_size > 0xFFFFFFFF) return -1;

    uint32_t file_size = (uint32_t)st.st_size;

    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    size_t alloc_size = (file_size == 0) ? 1 : (size_t)file_size;
    char *buf = (char *)malloc(alloc_size);
    if (!buf) { fclose(f); return -1; }

    if (file_size > 0) {
        size_t r = fread(buf, 1, file_size, f);
        if (r != (size_t)file_size) {
            free(buf);
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    *out_buf = buf;
    *out_len = file_size;
    return 0;
}



// Handle client request 
/* rewritten handle_client using parser */
int handle_client(int client_fd)
{
    Request req;
    if (parse_request(client_fd, &req) < 0) {
        const char *msg = "Malformed request";
        send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
        free_request(&req);
        return -1; /* close bad client */
    }

   switch (req.type) 
   {

    case CMD_SET: {
    if (store_set_string(req.key, req.value ? req.value : "") == 0) {
        const char *msg = "Key set";
        send_response(client_fd, STATUS_OK, msg, (uint32_t)strlen(msg));
    } else {
        const char *msg = "Set failed";
        send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
    }
    break;
      }

    case CMD_SETFILE: {
        /* Pass client_fd so the function reads directly from the socket */
        if (store_set_file(client_fd, req.key, req.val_len) == 0) {
            const char *msg = "File stored";
            send_response(client_fd, STATUS_OK, msg, (uint32_t)strlen(msg));
        } else {
            const char *msg = "Store failed";
            send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
        }
        break;
    }
    

    case CMD_GET: {
    const char *val = NULL;
    if (store_get_string(req.key, &val) == 0) {
        send_response(client_fd, STATUS_OK, val, (uint32_t)strlen(val));
    } else {
        const char *msg = "Not found";
        send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
    }
    break;
    }

    case CMD_GETFILE: {
    char *buf = NULL;
    uint32_t len = 0;

    if (store_get_file(req.key, &buf, &len) != 0) {
        const char *msg = "Not found";
        send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
        break;
    }

    
    send_response(client_fd, STATUS_OK, buf, len);
    free(buf);
    break;
    }

    case CMD_DEL: {
    char *old = (char *)ht_get(global_table, req.key);
    if (!old) {
        const char *msg = "Not found";
        send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
        break;
    }

    if (ht_delete(global_table, req.key)) {
        
        delete_if_file_value(old);
        free(old);

        const char *msg = "Key deleted";
        send_response(client_fd, STATUS_OK, msg, (uint32_t)strlen(msg));
    } else {
        const char *msg = "Delete failed";
        send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
    }
    break;
    }

case CMD_SEARCH:
case CMD_EXPIRE: {
    const char *msg = "Not implemented yet";
    send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
    break;
}

default: {
    const char *msg = "Unknown command";
    send_response(client_fd, STATUS_ERR, msg, (uint32_t)strlen(msg));
    break;
}
}

    free_request(&req);
    return 0;
}
// parse client request from the socket into a Request struct 

static void free_request(Request *req) {
    if(req == NULL) return;
    if (req->key) free(req->key);
    if (req->value) free(req->value);
    if (req->path) free(req->path);
}
static int read_bytes_or_fail(int fd, char *buf, size_t len) {
    if (recv_all(fd, buf, len) < 0) {
        return -1;
    }
    return 0;
}
static int parse_request(int client_fd, Request *req) {
    memset(req, 0, sizeof(*req));

    /* 1) type */
    ssize_t r = recv(client_fd, &req->type, 1, 0);
    if (r <= 0) return -1;

    /* 2) key_len */
    if (recv_u32(client_fd, &req->key_len) != 0) return -1;
    if (req->key_len == 0 || req->key_len > MAX_KEY_LEN) return -1;

    /* 3) key bytes */
    req->key = (char *)malloc(req->key_len + 1);
    if (!req->key) return -1;
    if (read_bytes_or_fail(client_fd, req->key, req->key_len) < 0) return -1;
    req->key[req->key_len] = '\0';

    /* 4) command-specific payload */
    switch (req->type) {
        case CMD_SET:
            if (recv_u32(client_fd, &req->val_len) != 0) return -1;
            if (req->val_len > MAX_VAL_LEN) return -1; // Limit string size

            req->value = (char *)malloc(req->val_len + 1);
            if (!req->value) return -1;
            if (read_bytes_or_fail(client_fd, req->value, req->val_len) < 0) return -1;
            req->value[req->val_len] = '\0';
            break;

        case CMD_SETFILE:
            /* Only read the file size, DO NOT read the file data into RAM here */
            if (recv_u32(client_fd, &req->val_len) != 0) return -1;
            break;

        case CMD_EXPIRE:
            if (recv_u32(client_fd, &req->ttl) != 0) return -1;
            if (req->ttl == 0) return -1;
            break;

        case CMD_GETFILE:
        case CMD_GET:
        case CMD_DEL:
        case CMD_SEARCH:
            /* key only */
            break;

        default:
            return -1;
    }

    return 0;
}