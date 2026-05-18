// helper.h
#ifndef HELPER_H
#define HELPER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include<stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/* will point to our hash table instance */
/* ------------------------------------------------------------------ */

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
#define CHUNK_SIZE    4096
/* Response status bytes */
#define STATUS_OK      0x00
#define STATUS_ERR     0xFF


/* ------------------------------------------------------------------ */
              
#define MAX_KEY_LEN    256
#define MAX_VAL_LEN    4096
#define MAX_PATH_LEN   512
/* ------------------------------------------------------------------ */
 
#ifndef FILE_STORE_DIR
#define FILE_STORE_DIR "store/files"
#endif




/* ------------------------------------------------------------------ */
/*Thread argument */ 
typedef struct {
    int      client_fd;
    int      epfd;
    char     key[MAX_KEY_LEN + 1];
    uint32_t val_len;
} FileThreadArg;
/* ------------------------------------------------------------------ */
/*requestes struct */ 
typedef struct {
    uint8_t  type;
    char    *key;
    uint32_t key_len;

    char    *value;      /* SET string or SETFILE bytes */
    uint32_t val_len;

    uint32_t ttl;        /* EXPIRE seconds */
} Request;
/* ------------------------------------------------------------------ */

int Socket(int family, int type, int protocol);
void Bind(int fd, const struct sockaddr *sa, socklen_t salen);
void Connect(int fd, const struct sockaddr *sa, socklen_t salen);
void Listen(int fd, int backlog);
int Accept(int fd, struct sockaddr *sa, socklen_t *salenptr);
void Close(int fd);
ssize_t Send(int fd, const void *ptr, size_t nbytes, int flags);
ssize_t Recv(int fd, void *ptr, size_t nbytes, int flags);
int make_non_blocking(int fd);
int make_listener(int port);
int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void *buf, size_t len);
int recv_u32(int fd, uint32_t *out);
int send_file(int client_fd, int file_fd, uint32_t file_size);
int send_response(int fd, uint8_t status,bool is_file, const char *data, uint32_t data_len);
int recv_file(int fd, const char *save_path, uint32_t file_size);
int write_all(int fd, const void *buf, size_t len);
void authentication(int new_socket, char * stats,size_t stats_size);


#endif // HELPER_H
