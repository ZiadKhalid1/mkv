/*
 * ============================================================
 *  Key-Value Store — Client
 *  Network Programming Course Project
 * ============================================================
 *
 *  Wire Protocol (Binary, Length-Prefixed):
 *
 *  Request:
 *  [1 byte : type]
 *  [4 bytes: key_len  (network byte order)]
 *  [N bytes: key]
 *  [4 bytes: val_len  (network byte order)]  -- omitted for GET/DEL/SEARCH
 *  [M bytes: value / raw file bytes]         -- omitted for GET/DEL/SEARCH
 *
 *  Command type bytes:
 *    0x01  SET     (string value)
 *    0x02  SETFILE (binary file)
 *    0x03  GET
 *    0x04  GETFILE
 *    0x05  DEL
 *    0x06  EXPIRE
 *    0x07  SEARCH
 *
 *  Response from server:
 *  [1 byte : status]   0x00 = OK, 0xFF = ERR
 *  [4 bytes: data_len] (network byte order)
 *  [N bytes: data]     (string message  OR  raw file bytes)
 *
 *  Compile:
 *    gcc -Wall -Wextra -o client client.c
 *
 *  Run:
 *    ./client                          (connects to 127.0.0.1:6379)
 *    ./client 192.168.1.10 7000        (custom host and port)
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* POSIX / socket headers */
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define DEFAULT_HOST   "127.0.0.1"
#define DEFAULT_PORT   6379

#define MAX_KEY_LEN    256
#define MAX_VAL_LEN    4096
#define MAX_PATH_LEN   512
#define CHUNK_SIZE     4096   /* bytes per chunk for file transfer     */

/* Command type bytes sent on the wire */
#define CMD_SET        0x01
#define CMD_SETFILE    0x02
#define CMD_GET        0x03
#define CMD_GETFILE    0x04
#define CMD_DEL        0x05
#define CMD_EXPIRE     0x06
#define CMD_SEARCH     0x07

/* Server response status bytes */
#define STATUS_OK      0x00
#define STATUS_ERR     0xFF

/* ANSI colour codes for nicer terminal output */
#define COL_RESET      "\033[0m"
#define COL_RED        "\033[31m"
#define COL_GREEN      "\033[32m"
#define COL_YELLOW     "\033[33m"
#define COL_CYAN       "\033[36m"
#define COL_BOLD       "\033[1m"

/* ------------------------------------------------------------------ */
/*  Data Structures                                                     */
/* ------------------------------------------------------------------ */

/* All information extracted from one line of user input */
typedef struct {
    uint8_t  type;                    /* CMD_* constant                */
    char     key[MAX_KEY_LEN];
    char     value[MAX_VAL_LEN];      /* used for SET string value     */
    char     filepath[MAX_PATH_LEN];  /* used for SETFILE / GETFILE    */
    uint32_t ttl;                     /* used for EXPIRE               */
} ParsedCommand;

/* ------------------------------------------------------------------ */
/*  Helper — print coloured text                                        */
/* ------------------------------------------------------------------ */

static void print_ok(const char *msg) {
    printf("%s%s[OK]%s %s\n", COL_BOLD, COL_GREEN, COL_RESET, msg);
}

static void print_err(const char *msg) {
    printf("%s%s[ERR]%s %s\n", COL_BOLD, COL_RED, COL_RESET, msg);
}

static void print_info(const char *msg) {
    printf("%s%s[INFO]%s %s\n", COL_BOLD, COL_CYAN, COL_RESET, msg);
}

/* ------------------------------------------------------------------ */
/*  Helper — reliable send / recv                                       */
/*                                                                      */
/*  TCP can split one send() into many recv() calls or merge several   */
/*  send() calls into one recv(). These wrappers keep reading /        */
/*  writing until every requested byte has been transferred.           */
/* ------------------------------------------------------------------ */

/* FIX #5: Separate error and closed-connection cases in send_all,
 * matching the cleaner pattern already used in recv_all. */
static int send_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *ptr = (const char *)buf;

    while (total < len) {
        ssize_t sent = send(fd, ptr + total, len - total, 0);
        if (sent < 0) {
            perror("send");
            return -1;
        }
        if (sent == 0) {
            print_err("Connection closed during send.");
            return -1;
        }
        total += (size_t)sent;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *ptr = (char *)buf;

    while (total < len) {
        ssize_t r = recv(fd, ptr + total, len - total, 0);
        if (r == 0) {
            print_err("Server closed the connection.");
            return -1;
        }
        if (r < 0) {
            perror("recv");
            return -1;
        }
        total += (size_t)r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helper — send a 4-byte unsigned integer in network byte order      */
/* ------------------------------------------------------------------ */

static int send_u32(int fd, uint32_t value) {
    uint32_t net = htonl(value);
    return send_all(fd, &net, 4);
}

/* ------------------------------------------------------------------ */
/*  Helper — receive a 4-byte unsigned integer and convert to host     */
/* ------------------------------------------------------------------ */

static int recv_u32(int fd, uint32_t *out) {
    uint32_t net;
    if (recv_all(fd, &net, 4) < 0) return -1;
    *out = ntohl(net);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Parser                                                              */
/*                                                                      */
/*  Accepts one line of text from stdin and fills a ParsedCommand.     */
/*  Supports quoted strings for values with spaces.                    */
/*                                                                      */
/*  Recognised commands:                                                */
/*    SET     <key> <value>                                             */
/*    SETFILE <key> <filepath>                                          */
/*    GET     <key>                                                     */
/*    GETFILE <key> <save_filepath>                                     */
/*    DEL     <key>                                                     */
/*    EXPIRE  <key> <seconds>                                           */
/*    SEARCH  <prefix>                                                  */
/*    HELP                                                              */
/*    EXIT / QUIT                                                       */
/*                                                                      */
/*  Returns:  0  on success                                             */
/*           -1  on parse error                                         */
/*            1  for HELP (handled inline)                              */
/*            2  for EXIT / QUIT                                        */
/* ------------------------------------------------------------------ */

static void show_help(void) {
    printf("\n%s%sAvailable Commands:%s\n", COL_BOLD, COL_YELLOW, COL_RESET);
    printf("  %-35s %s\n", "SET <key> <value>",         "Store a string value");
    printf("  %-35s %s\n", "SETFILE <key> <filepath>",  "Store a file");
    printf("  %-35s %s\n", "GET <key>",                 "Retrieve a string value");
    printf("  %-35s %s\n", "GETFILE <key> <savepath>",  "Download a stored file");
    printf("  %-35s %s\n", "DEL <key>",                 "Delete a key");
    printf("  %-35s %s\n", "EXPIRE <key> <seconds>",    "Set TTL on a key (not yet implemented)");
    printf("  %-35s %s\n", "SEARCH <prefix>",           "Find keys with prefix (not yet implemented)");
    printf("  %-35s %s\n", "HELP",                      "Show this message");
    printf("  %-35s %s\n", "EXIT / QUIT",               "Disconnect and exit");
    printf("\n");
}

/*
 * Extract the next token from *src, honouring double-quoted strings.
 * Writes the token into `dest` (max `dest_size` bytes incl. NUL).
 * Advances *src past the token and any trailing whitespace.
 * Returns 0 on success, -1 if no token found.
 */
static int next_token(const char **src, char *dest, size_t dest_size) {
    const char *p = *src;

    /* skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\0' || *p == '\n' || *p == '\r') return -1; /* nothing left */

    size_t i = 0;

    if (*p == '"') {
        /* quoted token — read until closing quote */
        p++; /* skip opening quote */
        while (*p && *p != '"' && *p != '\n') {
            if (i + 1 < dest_size) dest[i++] = *p;
            p++;
        }
        if (*p == '"') p++; /* skip closing quote */
    } else {
        /* plain token — read until whitespace */
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            if (i + 1 < dest_size) dest[i++] = *p;
            p++;
        }
    }

    dest[i] = '\0';

    /* skip trailing whitespace */
    while (*p == ' ' || *p == '\t') p++;
    *src = p;

    return (i == 0) ? -1 : 0;
}

static int parse_input(const char *line, ParsedCommand *cmd) {
    memset(cmd, 0, sizeof(*cmd));

    const char *p = line;
    char token[MAX_KEY_LEN];

    /* --- command word -------------------------------------------- */
    if (next_token(&p, token, sizeof(token)) < 0) return -1;

    /* uppercase the command word for case-insensitive matching */
    for (char *t = token; *t; t++)
        if (*t >= 'a' && *t <= 'z') *t -= 32;

    /* --- HELP / EXIT / QUIT -------------------------------------- */
    if (strcmp(token, "HELP") == 0) { show_help(); return 1; }
    if (strcmp(token, "EXIT") == 0 ||
        strcmp(token, "QUIT") == 0) return 2;

    /* --- commands that require at least a key ------------------- */
    if      (strcmp(token, "SET")     == 0) cmd->type = CMD_SET;
    else if (strcmp(token, "SETFILE") == 0) cmd->type = CMD_SETFILE;
    else if (strcmp(token, "GET")     == 0) cmd->type = CMD_GET;
    else if (strcmp(token, "GETFILE") == 0) cmd->type = CMD_GETFILE;
    else if (strcmp(token, "DEL")     == 0) cmd->type = CMD_DEL;
    else if (strcmp(token, "EXPIRE")  == 0) cmd->type = CMD_EXPIRE;
    else if (strcmp(token, "SEARCH")  == 0) cmd->type = CMD_SEARCH;
    else {
        printf("Unknown command: '%s'  (type HELP for a list)\n", token);
        return -1;
    }

    /* --- key / prefix -------------------------------------------- */
    if (next_token(&p, cmd->key, MAX_KEY_LEN) < 0) {
        print_err("Missing key.");
        return -1;
    }

    /* --- additional arguments depending on command --------------- */
    switch (cmd->type) {

        case CMD_SET:
            if (next_token(&p, cmd->value, MAX_VAL_LEN) < 0) {
                print_err("SET requires a value.");
                return -1;
            }
            break;

        case CMD_SETFILE:
            if (next_token(&p, cmd->filepath, MAX_PATH_LEN) < 0) {
                print_err("SETFILE requires a filepath.");
                return -1;
            }
            break;

        case CMD_GETFILE:
            if (next_token(&p, cmd->filepath, MAX_PATH_LEN) < 0) {
                print_err("GETFILE requires a save path.");
                return -1;
            }
            break;

        case CMD_EXPIRE: {
            char ttl_str[32];
            if (next_token(&p, ttl_str, sizeof(ttl_str)) < 0) {
                print_err("EXPIRE requires <seconds>.");
                return -1;
            }
            int ttl = atoi(ttl_str);
            if (ttl <= 0) {
                print_err("EXPIRE seconds must be a positive integer.");
                return -1;
            }
            cmd->ttl = (uint32_t)ttl;
            break;
        }

        /* GET / DEL / SEARCH need nothing beyond the key */
        default:
            break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Progress Bar                                                        */
/* ------------------------------------------------------------------ */

static void print_progress(const char *label, size_t done, size_t total) {
    int pct  = (int)((double)done / (double)total * 100.0);
    int bars = pct / 5;   /* 20 bar segments */

    printf("\r%s [", label);
    for (int i = 0; i < 20; i++)
        putchar(i < bars ? '#' : '-');
    printf("] %3d%%  (%zu / %zu bytes)   ", pct, done, total);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/*  Send helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * Send:  [type][key_len][key]
 * Used by GET, DEL, SEARCH — no value field.
 */
static int send_key_only(int fd, uint8_t type, const char *key) {
    uint32_t key_len = (uint32_t)strlen(key);

    if (send_all(fd, &type, 1)     < 0) return -1;
    if (send_u32(fd, key_len)      < 0) return -1;
    if (send_all(fd, key, key_len) < 0) return -1;
    return 0;
}

/*
 * Send:  [type][key_len][key][val_len][value_string]
 */
static int send_string_value(int fd, const char *key, const char *value) {
    uint8_t  type    = CMD_SET;
    uint32_t key_len = (uint32_t)strlen(key);
    uint32_t val_len = (uint32_t)strlen(value);

    if (send_all(fd, &type, 1)       < 0) return -1;
    if (send_u32(fd, key_len)        < 0) return -1;
    if (send_all(fd, key, key_len)   < 0) return -1;
    if (send_u32(fd, val_len)        < 0) return -1;
    if (send_all(fd, value, val_len) < 0) return -1;
    return 0;
}

/*
 * Send:  [type][key_len][key][file_size][raw binary data in chunks]
 *
 * FIX #1: Validate file size fits in uint32_t before casting.
 * FIX #2: Detect fread() errors separately from EOF.
 */
static int send_file(int fd, const char *key, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "%s[ERR]%s Cannot open file '%s': %s\n",
                COL_RED, COL_RESET, filepath, strerror(errno));
        return -1;
    }

    /* get file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        print_err("Could not seek to end of file.");
        fclose(f);
        return -1;
    }
    long file_size_signed = ftell(f);
    rewind(f);

    if (file_size_signed < 0) {
        print_err("Could not determine file size.");
        fclose(f);
        return -1;
    }

    /* FIX #1: Reject files larger than 4 GB (uint32_t max) */
    if (file_size_signed > (long)UINT32_MAX) {
        print_err("File too large — maximum supported size is 4 GB.");
        fclose(f);
        return -1;
    }

    uint32_t file_size = (uint32_t)file_size_signed;
    uint8_t  type      = CMD_SETFILE;
    uint32_t key_len   = (uint32_t)strlen(key);

    /* send header */
    if (send_all(fd, &type, 1)     < 0) { fclose(f); return -1; }
    if (send_u32(fd, key_len)      < 0) { fclose(f); return -1; }
    if (send_all(fd, key, key_len) < 0) { fclose(f); return -1; }
    if (send_u32(fd, file_size)    < 0) { fclose(f); return -1; }

    /* send file data in chunks */
    char   chunk[CHUNK_SIZE];
    size_t total_sent = 0;
    size_t bytes_read;

    while ((bytes_read = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (send_all(fd, chunk, bytes_read) < 0) {
            fclose(f);
            return -1;
        }
        total_sent += bytes_read;
        print_progress("Uploading", total_sent, file_size);
    }

    /* FIX #2: Distinguish a real read error from normal EOF */
    if (ferror(f)) {
        print_err("File read error during upload.");
        fclose(f);
        return -1;
    }

    fclose(f);
    printf("\n");   /* newline after progress bar */
    return 0;
}

/*
 * Send:  [type][key_len][key]
 * The server looks up the key and streams the file back.
 * The save path is chosen by the client locally — not sent to the server.
 *
 * FIX #3: Removed dead commented-out code from an older protocol design.
 */
static int send_getfile_request(int fd, const char *key) {
    uint8_t  type    = CMD_GETFILE;
    uint32_t key_len = (uint32_t)strlen(key);

    if (send_all(fd, &type, 1)     < 0) return -1;
    if (send_u32(fd, key_len)      < 0) return -1;
    if (send_all(fd, key, key_len) < 0) return -1;
    return 0;
}

/*
 * Send:  [type][key_len][key][ttl (4 bytes)]
 */
static int send_expire(int fd, const char *key, uint32_t ttl) {
    uint8_t  type    = CMD_EXPIRE;
    uint32_t key_len = (uint32_t)strlen(key);

    if (send_all(fd, &type, 1)     < 0) return -1;
    if (send_u32(fd, key_len)      < 0) return -1;
    if (send_all(fd, key, key_len) < 0) return -1;
    if (send_u32(fd, ttl)          < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Receive response from server                                        */
/* ------------------------------------------------------------------ */

/*
 * Every server response starts with:
 *   [1 byte: status]  0x00 = OK, 0xFF = ERR
 *   [4 bytes: data_len]
 *   [data_len bytes: payload]
 *
 * For GET / SEARCH the payload is a printable string.
 * For GETFILE the payload is raw binary file data.
 * For SET / DEL / EXPIRE the payload is a short status message.
 */
static int receive_response(int fd, int is_file, const char *save_path) {
    uint8_t  status;
    uint32_t data_len;

    /* read status byte */
    if (recv_all(fd, &status, 1) < 0)  return -1;

    /* read payload length */
    if (recv_u32(fd, &data_len) < 0)   return -1;

    /* --- error response ------------------------------------------ */
    if (status == STATUS_ERR) {
        char *msg = (char *)malloc(data_len + 1);
        if (!msg) { print_err("Out of memory."); return -1; }
        if (recv_all(fd, msg, data_len) < 0) { free(msg); return -1; }
        msg[data_len] = '\0';
        print_err(msg);
        free(msg);
        return -1;
    }

    /* --- file response ------------------------------------------- */
    if (is_file) {
        FILE *f = fopen(save_path, "wb");
        if (!f) {
            fprintf(stderr, "%s[ERR]%s Cannot create file '%s': %s\n",
                    COL_RED, COL_RESET, save_path, strerror(errno));
            /* drain the bytes from the socket so the stream stays clean */
            char drain[CHUNK_SIZE];
            size_t remaining = data_len;
            while (remaining > 0) {
                size_t  to_read = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
                ssize_t r       = recv(fd, drain, to_read, 0);
                if (r <= 0) break;
                remaining -= (size_t)r;
            }
            return -1;
        }

        size_t total_received = 0;
        char   chunk[CHUNK_SIZE];

        while (total_received < data_len) {
            size_t  want = data_len - total_received;
            if (want > CHUNK_SIZE) want = CHUNK_SIZE;

            ssize_t r = recv(fd, chunk, want, 0);
            if (r <= 0) { fclose(f); return -1; }

            fwrite(chunk, 1, (size_t)r, f);
            total_received += (size_t)r;
            print_progress("Downloading", total_received, data_len);
        }

        fclose(f);
        printf("\n");
        printf("%s[OK]%s File saved to '%s'  (%u bytes)\n",
               COL_GREEN, COL_RESET, save_path, data_len);
        return 0;
    }

    /* --- string / message response ------------------------------- */
    if (data_len == 0) {
        print_ok("(empty value)");
        return 0;
    }

    char *msg = (char *)malloc(data_len + 1);
    if (!msg) { print_err("Out of memory."); return -1; }
    if (recv_all(fd, msg, data_len) < 0) { free(msg); return -1; }
    msg[data_len] = '\0';
    print_ok(msg);
    free(msg);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Dispatch — call the right send function then read the response     */
/* ------------------------------------------------------------------ */

static int dispatch(int fd, const ParsedCommand *cmd) {
    int rc = 0;

    switch (cmd->type) {

        case CMD_SET:
            rc = send_string_value(fd, cmd->key, cmd->value);
            if (rc == 0) rc = receive_response(fd, 0, NULL);
            break;

        case CMD_SETFILE:
            rc = send_file(fd, cmd->key, cmd->filepath);
            if (rc == 0) rc = receive_response(fd, 0, NULL);
            break;

        case CMD_GET:
            rc = send_key_only(fd, CMD_GET, cmd->key);
            if (rc == 0) rc = receive_response(fd, 0, NULL);
            break;

        case CMD_GETFILE:
            rc = send_getfile_request(fd, cmd->key);
            if (rc == 0) rc = receive_response(fd, 1, cmd->filepath);
            break;

        case CMD_DEL:
            rc = send_key_only(fd, CMD_DEL, cmd->key);
            if (rc == 0) rc = receive_response(fd, 0, NULL);
            break;

        case CMD_EXPIRE:
            rc = send_expire(fd, cmd->key, cmd->ttl);
            if (rc == 0) rc = receive_response(fd, 0, NULL);
            break;

        case CMD_SEARCH:
            rc = send_key_only(fd, CMD_SEARCH, cmd->key);
            if (rc == 0) rc = receive_response(fd, 0, NULL);
            break;

        default:
            print_err("Unknown command type in dispatch (this is a bug).");
            rc = -1;
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/*  Connect to server                                                   */
/* ------------------------------------------------------------------ */

static int connect_to_server(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/*  Main — CLI event loop                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    const char *host = DEFAULT_HOST;
    int         port = DEFAULT_PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    printf("%sConnecting to %s:%d ...%s\n", COL_CYAN, host, port, COL_RESET);

    int fd = connect_to_server(host, port);
    if (fd < 0) {
        fprintf(stderr, "Could not connect to server.\n");
        return 1;
    }

    printf("%s%sConnected!%s  Type HELP for available commands.\n\n",
           COL_BOLD, COL_GREEN, COL_RESET);

    /* ---- REPL loop ------------------------------------------------ */
    /*
     * Buffer is sized for the longest possible command line:
     *   "SETFILE " (8) + key (MAX_KEY_LEN) + " " + path (MAX_PATH_LEN) + newline + NUL
     * MAX_VAL_LEN is larger than MAX_PATH_LEN so using it keeps us safe
     * for SET commands too. Do not shrink this without checking both.
     */
    char          line[MAX_KEY_LEN + MAX_VAL_LEN + 64];
    ParsedCommand cmd;

while (1) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);  // watch keyboard
    FD_SET(fd, &readfds);            // watch socket

    printf("%skvstore>%s ", COL_YELLOW, COL_RESET);
    fflush(stdout);

    int ready = select(fd + 1, &readfds, NULL, NULL, NULL);
    if (ready < 0) {
        perror("select");
        break;
    }

    // socket became readable — means server closed the connection
    if (FD_ISSET(fd, &readfds)) {
        printf("\n%s[INFO]%s Server disconnected.\n", COL_BOLD, COL_RESET);
        break;
    }

    // keyboard input is ready
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\r' || *p == '\0') continue;

        int parse_rc = parse_input(line, &cmd);
        if (parse_rc == 1) continue;
        if (parse_rc == 2) break;
        if (parse_rc < 0)  continue;

        dispatch(fd, &cmd);
    }
}

// ---- clean up -------------------------------------------------
    close(fd);
    printf("%sDisconnected. Goodbye!%s\n", COL_CYAN, COL_RESET);
    return 0;
}