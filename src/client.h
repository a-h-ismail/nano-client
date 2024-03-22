#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

#include <config.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>

typedef struct sockaddr SA;

typedef enum rt_command
{
    STATE,
    ADD_LINE,
    APPEND_LINE,
    END_APPEND,
    ADD_STR,
    REMOVE_STR,
    REMOVE_LINE,
    MOVE_CURSOR
} rt_command;

typedef struct payload
{
    uint8_t user_id;
    rt_command function;
    char *data;
    uint16_t data_size;
} payload;

// Read the data at ptr to the variable var
#define READ_BIN(var, ptr) memcpy(&var, ptr, sizeof(var));

// Write the variable var to address ptr (even if unaligned)
#define WRITE_BIN(var, ptr) memcpy(ptr, &var, sizeof(var));

extern bool remote_buffer;
extern bool download_done;

int read_data(void *_dest, int N);

int read_n(int fd, void *b, size_t n);

int write_data(void *_dest, int N);

void start_client();

void report_cursor_move();

extern pthread_mutex_t lock_openfile;

#endif