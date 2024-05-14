#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

#include <config.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include "definitions.h"

typedef struct sockaddr SA;

typedef enum rt_command
{
    ADD_USER,
    REMOVE_USER,
    ADD_LINE,
    REMOVE_LINE,
    REPLACE_LINE,
    BREAK_LINE,
    APPEND_LINE,
    END_APPEND,
    ADD_STR,
    REMOVE_STR,
    MOVE_CURSOR,
    OPEN_FILE,
    STATUS
} rt_command;

enum status_msg
{
    ACCEPTED,
    FILE_INACCESSIBLE,
    CLIENTS_EXCEEDED,
    PROTOCOL_ERROR
};

typedef struct payload
{
    uint16_t data_size;
    int8_t user_id;
    rt_command function;
    char data[1024];
} payload;

typedef struct client_data
{
    char *name;
    linestruct *current_line;
    int32_t xpos;
    int8_t user_id;
} client_data;

// Read the data at ptr to the variable var
#define READ_BIN(var, ptr) memcpy(&var, ptr, sizeof(var))

// Write the variable var to address ptr (even if unaligned)
#define WRITE_BIN(var, ptr) memcpy(ptr, &var, sizeof(var))

extern bool remote_buffer;
extern bool download_done;

extern client_data clients[];
extern int client_count;

// Checks if the function f is supported in remote operations
bool function_remote_compatible(void *f);

int read_data(void *_dest, int N);

int read_n(int fd, void *b, size_t n);

int write_data(void *_dest, int N);

void start_client();

void *report_cursor_move(void *);

void draw_remote_cursors();

void report_insertion(char *burst);

void report_deletion(bool is_backspace);

void report_enter(bool first_call);

extern pthread_mutex_t lock_openfile;

extern openfilestruct *openfile;

extern char *server_ip;

extern char *alternate_title;

extern char *remote_filename;

#endif