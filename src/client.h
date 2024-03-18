#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

typedef struct sockaddr SA;

extern bool remote_buffer;

int read_data(void *_dest, int N);

int write_data(void *_dest, int N);

void start_client();

extern pthread_mutex_t lock_buffer;

#endif