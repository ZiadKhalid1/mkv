# ==========================================
# Key-Value Store Makefile
# ==========================================

CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -O2 -g -Iinclude

# --- DIRECTORIES ---
SRC_DIR = src
BIN_DIR = bin

# --- SERVER FILES ---
SERVER_SRCS = $(SRC_DIR)/server.c $(SRC_DIR)/helper.c $(SRC_DIR)/ht.c $(SRC_DIR)/database.c $(SRC_DIR)/ttl.c $(SRC_DIR)/LRU_linked_list.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)
SERVER_BIN = $(BIN_DIR)/server

# --- CLIENT FILES ---
CLIENT_SRCS = $(SRC_DIR)/client.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)
CLIENT_BIN = $(BIN_DIR)/client

# ------------------------------------------

# Default rule: Build both server and client
all: $(SERVER_BIN) $(CLIENT_BIN)

# Link the Server
$(SERVER_BIN): $(SERVER_OBJS)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(SERVER_OBJS) -o $(SERVER_BIN)

# Link the Client
$(CLIENT_BIN): $(CLIENT_OBJS)
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CLIENT_OBJS) -o $(CLIENT_BIN)

# Compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up all compiled files and the bin directory
clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS)
	rm -rf $(BIN_DIR)

.PHONY: all clean
