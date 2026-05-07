#include "../include/helper.h"
#include "../include/ttl.h"

ht *global_table = NULL;
/*functions prototype*/
static int parse_request(int client_fd, Request *req);
static void free_request(Request *req);
static void epoll_loop(int server_fd);
static int handle_client(int client_fd);
static uint64_t fnv1a64(const char *s);
static int ensure_dir(const char *dir);
static int ensure_store_dirs(void);
static int store_set_string(const char *key, const char *value);
static int store_set_file(int client_fd, const char *key, uint32_t file_size);
static int get_string(const char *key, const char **out_val);
static int get_file(const char *key, char **out_buf, uint32_t *out_len);

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
  if (mkdir(dir, 0755) == 0)
    return 0;
  if (errno == EEXIST)
    return 0;
  return -1;
}

/* Ensure store/files exists (create parent then child) */
static int ensure_store_dirs(void) {
  if (ensure_dir("store") != 0)
    return -1;
  if (ensure_dir(FILE_STORE_DIR) != 0)
    return -1;
  return 0;
}

int main(int argc, char *argv[]) {
  // Ignore SIGPIPE to prevent crashes when client drops connection during
  signal(SIGPIPE, SIG_IGN);

  int port = DEFAULT_PORT;

  global_table = db_init();
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
    if (p > 1024 && p < 49151)
      port = p;
    else
      printf("Using default port %d\n", DEFAULT_PORT);
  } else {
    printf("Using default port %d\n", DEFAULT_PORT);
  }

  int server_fd = make_listener(port);
  printf("Server is listening on port %d...\n", port);

  make_non_blocking(server_fd);
  if (server_fd < 0) {
    fprintf(stderr, "Failed to create server socket\n");
    return EXIT_FAILURE;
  }

  epoll_loop(server_fd);

  db_destroy(global_table);
  return 0;
}

/* ------------------------------------------------------------------ */
// functions  implementations
/* ------------------------------------------------------------------ */

// Epoll loop implementation
void epoll_loop(int server_fd) {
  int epfd = epoll_create1(0);
  if (epfd < 0) {
    perror("epoll_create1");
    return;
  }

  struct epoll_event ev, events[MAX_CLIENTS];
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN; // level-triggered
  ev.data.fd = server_fd;

  if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
    perror("epoll_ctl ADD server_fd");
    Close(epfd);
    return;
  }

  while (1) {
    int timeout = ttl_get_next_timeout();
    int nfds = epoll_wait(epfd, events, MAX_CLIENTS, timeout);
    if (nfds < 0) {
      if (errno == EINTR)
        continue; // interrupted by signal
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
          ev.events = EPOLLIN; // keep level-triggered
          ev.data.fd = client_fd;

          if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            perror("epoll_ctl ADD client_fd");
            close(client_fd);
            continue;
          }

          printf("New client connected: fd %d\n", client_fd);
        }
      } else {
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
    ttl_process_expirations(global_table);
  }

  Close(epfd);
}
// store a string value in the hash table under the given key

static int store_set_string(const char *key, const char *value) {
  if (!key || !value)
    return -1;

  size_t vlen = strlen(value);
  char *entry = (char *)malloc(vlen + 1);
  if (!entry)
    return -1;

  memcpy(entry, value, vlen + 1);
  if (!set_n(global_table, key, entry, false)) {
    free(entry);
    return -1;
  }
  return 0;
}
// store a file's contents in the hash table under the given key
static int store_set_file(int client_fd, const char *key, uint32_t file_size) {
  if (!key)
    return -1;

  char filepath[MAX_PATH_LEN];
  char tmpfile[MAX_PATH_LEN];
  uint64_t h = fnv1a64(key);

  snprintf(filepath, sizeof(filepath), "%s/%016llx.bin", FILE_STORE_DIR,
           (unsigned long long)h);
  snprintf(tmpfile, sizeof(tmpfile), "%s/%016llx.tmp", FILE_STORE_DIR,
           (unsigned long long)h);

  if (recv_file(client_fd, tmpfile, file_size) < 0) {
    remove(tmpfile);
    return -1;
  }

  if (rename(tmpfile, filepath) != 0) {
    remove(tmpfile);
    perror("rename store_set_file");
    return -1;
  }

  size_t plen = strlen(filepath);
  char *entry = (char *)malloc(plen + 1);
  if (!entry) {
    remove(filepath);
    return -1;
  }
  memcpy(entry, filepath, plen + 1);

  if (!set_n(global_table, key, entry, true)) {
    free(entry);
    remove(filepath);
    return -1;
  }

  return 0;
}
// retrieve a string value from the hash table under the given key
static int get_string(const char *key, const char **out_val) {
  char *entry = (char *)get_v(global_table, key);
  if (!entry)
    return -1;
  if (is_file(global_table, key))
    return -1; /* it's a file */
  *out_val = entry;
  return 0;
}
// retrieve a file's contents from the hash table under the given key
static int get_file(const char *key, char **file_path, uint32_t *file_size) {
  char *entry = (char *)get_v(global_table, key);
  if (!entry)
    return -1;

  if (!is_file(global_table, key))
    return -1; /* it's a string */

  const char *path = entry;
  // Validate file existence and size before returning path
  struct stat st;
  if (stat(path, &st) != 0)
    return -1; // file doesn't exist
  if (!S_ISREG(st.st_mode))
    return -1;
  if (st.st_size < 0 || st.st_size > 0xFFFFFFFF)
    return -1;
  *file_path = strdup(path);
  if (!*file_path)
    return -1;
  *file_size = (uint32_t)st.st_size;
  return 0;
}
// parse client request from the socket into a Request struct

static void free_request(Request *req) {
  if (req == NULL)
    return;
  if (req->key)
    free(req->key);
  if (req->value)
    free(req->value);
}

static int parse_request(int client_fd, Request *req) {
  memset(req, 0, sizeof(*req));

  /* 1) type */
  if (recv_all(client_fd, &req->type, 1) < 0)
    return -1;

  /* 2) key_len */
  if (recv_u32(client_fd, &req->key_len) != 0)
    return -1;
  if (req->key_len == 0 || req->key_len > MAX_KEY_LEN)
    return -1;

  /* 3) key bytes */
  req->key = (char *)malloc(req->key_len + 1);
  if (!req->key)
    return -1;
  if (recv_all(client_fd, req->key, req->key_len) < 0)
    return -1;
  req->key[req->key_len] = '\0';

  /* 4) command-specific payload */
  switch (req->type) {
  case CMD_SET:
    if (recv_u32(client_fd, &req->val_len) != 0)
      return -1;
    if (req->val_len > MAX_VAL_LEN)
      return -1; // Limit string size

    req->value = (char *)malloc(req->val_len + 1);
    if (!req->value)
      return -1;
    if (recv_all(client_fd, req->value, req->val_len) < 0)
      return -1;
    req->value[req->val_len] = '\0';
    break;

  case CMD_SETFILE:
    /* Only read the file size, DO NOT read the file data into RAM here */
    if (recv_u32(client_fd, &req->val_len) != 0)
      return -1;
    break;

  case CMD_EXPIRE:
    if (recv_u32(client_fd, &req->ttl) != 0)
      return -1;
    if (req->ttl == 0)
      return -1;
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

// Handle client request
/* rewritten handle_client using parser */
int handle_client(int client_fd) {
  Request req;
  if (parse_request(client_fd, &req) < 0) {
    const char *msg = "Malformed request";
    send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    free_request(&req);
    return -1; /* close bad client */
  }

  switch (req.type) {

  case CMD_SET: {
    if (store_set_string(req.key, req.value ? req.value : "") == 0) {
      const char *msg = "Key set";
      send_response(client_fd, STATUS_OK, false, msg, (uint32_t)strlen(msg));
    } else {
      const char *msg = "Set failed";
      send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    }
    break;
  }

  case CMD_SETFILE: {
    /* Pass client_fd so the function reads directly from the socket */
    if (store_set_file(client_fd, req.key, req.val_len) == 0) {
      const char *msg = "File stored";
      send_response(client_fd, STATUS_OK, false, msg, (uint32_t)strlen(msg));
    } else {
      const char *msg = "Store failed";
      send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    }
    break;
  }

  case CMD_GET: {
    const char *val = NULL;
    if (get_string(req.key, &val) == 0) {
      send_response(client_fd, STATUS_OK, false, val, (uint32_t)strlen(val));
    } else {
      const char *msg = "Not found";
      send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    }
    break;
  }

  case CMD_GETFILE: {
    char *file_path = NULL;
    uint32_t file_size = 0;

    if (get_file(req.key, &file_path, &file_size) != 0) {
      const char *msg = "Not found";
      send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
      break;
    }

    send_response(client_fd, STATUS_OK, true, file_path, file_size);
    free(file_path); // free the duplicated file path string from hash table
    break;
  }

  case CMD_DEL: {
    if (delete_n(global_table, req.key)) {

      const char *msg = "Key deleted";
      send_response(client_fd, STATUS_OK, false, msg, (uint32_t)strlen(msg));
    } else {
      const char *msg = "these key not found";
      send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    }
    break;
  }

  case CMD_SEARCH: {
    const char *msg = "Not implemented yet";
    send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    break;
  }
  case CMD_EXPIRE: {
    if (set_ttl(global_table, req.key, req.ttl)) {
      const char *msg = "the key will removed after the ttl";
      send_response(client_fd, STATUS_OK, false, msg, (uint32_t)strlen(msg));
    } else {
      const char *msg = "these key not found";
      send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    }
    break;
  }

  default: {
    const char *msg = "Unknown command";
    send_response(client_fd, STATUS_ERR, false, msg, (uint32_t)strlen(msg));
    break;
  }
  }

  free_request(&req);
  return 0;
}
