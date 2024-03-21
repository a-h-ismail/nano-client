#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

typedef struct sockaddr SA;

extern bool remote_buffer;
extern bool download_done;

int read_data(void *_dest, int N);

int write_data(void *_dest, int N);

void start_client();

extern pthread_mutex_t lock_openfile;

typedef struct payload
{
    uint8_t user_id;
    rt_command function;
    char *data;
} payload;

#endif