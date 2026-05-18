#include "../include/helper.h"
#include "../include/ht.h"
#include <sqlite3.h>


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
int recv_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *ptr = (char *)buf;
    
    while (total < len) {
        ssize_t r = recv(fd, ptr + total, len - total, 0);
        
        if (r == 0) { 
            printf("[INFO] Client disconnected.\n"); 
            return -1; 
        }
        
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait for socket to be readable
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(fd, &readfds);
                
                int ret = select(fd + 1, &readfds, NULL, NULL, NULL);
                if (ret < 0) {
                    perror("select error");
                    return -1;
                }
                continue;  // Retry recv
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
int send_file(int client_fd, int file_fd, uint32_t file_size) {
    uint32_t total = 0;
    while (total < file_size) {
        uint32_t read_bytes = (file_size - total > CHUNK_SIZE) ? CHUNK_SIZE : file_size - total;
        char chunk[CHUNK_SIZE];
        
        ssize_t r = read(file_fd, chunk, (size_t)read_bytes);
        if (r < 0) {
            perror("read file error");
            return -1;
        }
        if (r == 0) {
            // EOF reached before expected file_size
            fprintf(stderr, "EOF reached prematurely\n");
            return -1;
        }
        
        if (send_all(client_fd, chunk, (size_t)r) < 0) return -1;
        total += (size_t)r;
    }
    return 0;
}
/* ------------------------------------------------------------------ */

int send_response(int client_fd, uint8_t status, bool isafile, const char *data, uint32_t data_len) {
    if (!data) {
        perror("send_response: data is NULL");
        return -1;
    }
    
    uint32_t len_net = htonl(data_len);
    if (send_all(client_fd, &status, 1) < 0) return -1;
    if (send_all(client_fd, &len_net, 4) < 0) return -1;
    
    if (isafile) {
        int file_fd = open(data, O_RDONLY);
        if (file_fd < 0) {
            perror("open file for sending error");
            return -1;
        }
        if (send_file(client_fd, file_fd, data_len) < 0) {
            close(file_fd);
            return -1;
        }
        close(file_fd);
    } else {
        if (send_all(client_fd, data, data_len) < 0) return -1;
    }
    return 0;
}

int send_all(int client_fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *ptr = (const char *)buf;
    
    while (total < len) {
        ssize_t s = send(client_fd, ptr + total, len - total, 0);
        if (s < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Wait for socket to be writable
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(client_fd, &writefds);
                
                int ret = select(client_fd + 1, NULL, &writefds, NULL, NULL);
                if (ret < 0) {
                    perror("select error");
                    return -1;
                }
                continue;  // Retry send
            }
            perror("send error");
            return -1;
        }
        
        if (s == 0) {
            return -1;  // Connection closed
        }
        
        total += (size_t)s;
    }
    return 0;
}

int write_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *ptr = (const char *)buf;
    
    while (total < len) {
        ssize_t s = write(fd, ptr + total, len - total);
        if (s < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // Wait for socket to be writable
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(fd, &writefds);
                
                int ret = select(fd + 1, NULL, &writefds, NULL, NULL);
                if (ret < 0) {
                    perror("select error");
                    return -1;
                }
                continue;  // Retry write
            }
            perror("write error");
            return -1;
        }
        
        if (s == 0) {
            return -1;  // Connection closed
        }
        
        total += (size_t)s;
    }
    return 0;
}

int recv_file(int client_fd, const char *save_path, uint32_t file_size) {
    int fd = open(save_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open file for writing error");
        return -1;
    }
    
    uint32_t total = 0;
    char buffer[CHUNK_SIZE];
    while (total < file_size) {
         size_t want = file_size - total;
        if (want > CHUNK_SIZE) want = CHUNK_SIZE;
        if (recv_all(client_fd, buffer, want) < 0) {
            close(fd);
            return -1;
        }
    if (write_all(fd, buffer, want) < 0) {
            close(fd);
            return -1;
        }
        
        total += (size_t)want;
    }
    
    close(fd);
    return 0;
}


//if successful return username if not return 'not found'
void authentication(int new_socket, char * stats,size_t stats_size)
 {
    strncpy(stats, "not found", stats_size - 1);
      
    char buffer[1024] = {0};
  
        int valread = read(new_socket,
                           buffer,
                           sizeof(buffer) - 1);

        if (valread <= 0) {

            perror("read");

            return;

        }

        // إضافة null character
        buffer[valread] = '\0';

        printf("Received: %s\n", buffer);

        // تقسيم البيانات
        char *saveptr;

        char *command =
            strtok_r(buffer, " \n", &saveptr);

        char *username =
            strtok_r(NULL, " \n", &saveptr);
     
        char *password =
            strtok_r(NULL, " \n", &saveptr);

        // التحقق من وجود command
        if (command == NULL) {

            send(new_socket,
                 "Invalid input\n",
                 strlen("Invalid input\n"),
                 0);

            return;
        }

        // التحقق من username و password
        if ((strcmp(command, "login") == 0 ||
             strcmp(command, "register") == 0)
             &&
            (username == NULL || password == NULL)) {

            send(new_socket,
                 "Username or password missing\n",
                 strlen("Username or password missing\n"),
                 0);

            return;
        }

        // فتح قاعدة البيانات
        sqlite3 *db;

        if (sqlite3_open("users.db", &db) != SQLITE_OK) {

            perror("Failed to open database");

           return;
        }

        // SQLite Statement
        sqlite3_stmt *stmt = NULL;

        // =========================================
        // LOGIN
        // =========================================

        if (strcmp(command, "login") == 0) {

            const char *sql =
                "SELECT * FROM users "
                "WHERE username = ? AND password = ?";

            if (sqlite3_prepare_v2(db,
                                   sql,
                                   -1,
                                   &stmt,
                                   NULL) != SQLITE_OK) {

                perror("prepare failed");
            }

            // ربط username
            sqlite3_bind_text(stmt,
                              1,
                              username,
                              -1,
                              SQLITE_TRANSIENT);

            // ربط password
            sqlite3_bind_text(stmt,
                              2,
                              password,
                              -1,
                              SQLITE_TRANSIENT);

            int rc = sqlite3_step(stmt);

            // لو وجد المستخدم
            if (rc == SQLITE_ROW) {

                printf("Login successful for user: %s\n",
                       username);

                char *message =
                    "Login successful!\n";
                strncpy(stats, username , stats_size - 1);
                send(new_socket,
                     message,
                     strlen(message),
                     0);

            } else {

                printf("Login failed\n");

                char *message =
                    "Login failed: wrong username or password\n";

                send(new_socket,
                     message,
                     strlen(message),
                     0);
            }
        }

        // =========================================
        // REGISTER
        // =========================================

        else if (strcmp(command, "register") == 0) {

            const char *sql =
                "INSERT INTO users(username, password) "
                "VALUES(?, ?)";

            if (sqlite3_prepare_v2(db,
                                   sql,
                                   -1,
                                   &stmt,
                                   NULL) != SQLITE_OK) {

                perror("prepare failed");
            }

            // ربط username
            sqlite3_bind_text(stmt,
                              1,
                              username,
                              -1,
                              SQLITE_TRANSIENT);

            // ربط password
            sqlite3_bind_text(stmt,
                              2,
                              password,
                              -1,
                              SQLITE_TRANSIENT);

            int rc = sqlite3_step(stmt);

            // نجاح التسجيل
            if (rc == SQLITE_DONE) {

                printf("Registration successful\n");

                char *message =
                    "Registration successful\n";

                send(new_socket,
                     message,
                     strlen(message),
                     0);

            } else {

                printf("Registration failed\n");

                char *message =
                    "User may already exist\n";

                send(new_socket,
                     message,
                     strlen(message),
                     0);
            }
        }

        // =========================================
        // EXIT
        // =========================================

        else if (strcmp(command, "exit") == 0) {

            char *message =
                "Connection closed\n";

            send(new_socket,
                 message,
                 strlen(message),
                 0);

            printf("Client disconnected\n");

            close(new_socket);

            sqlite3_close(db);

         
        }

        // =========================================
        // INVALID COMMAND
        // =========================================

        else {

            char *message =
                "Invalid command\n";

            send(new_socket,
                 message,
                 strlen(message),
                 0);
        }

        // تنظيف الـ statement
        if (stmt != NULL) {

            sqlite3_finalize(stmt);
        }

        // غلق قاعدة البيانات
        sqlite3_close(db);

        printf("Connection closed\n");
    return;
 }

