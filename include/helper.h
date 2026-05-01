// helper.h
#ifndef HELPER_H
#define HELPER_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#ifndef BACKLOG
#define BACKLOG SOMAXCONN
#endif

#ifndef CHUNK_SIZE
#define CHUNK_SIZE 4096
#endif

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
int send_response(int fd, uint8_t status, const char *data, uint32_t data_len);
int recv_all_file(int fd, const char *save_path, uint32_t file_size);


#endif // HELPER_H
