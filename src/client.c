#include <config.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include "client.h"
#include "prototypes.h"

int b_start;
int b_current;
bool remote_buffer = false;
bool download_done = false;

#define TC_BUF_SIZE 1024
// Used for thread communication
char tc_buffer[TC_BUF_SIZE];
void process_commands(char *commands);
pthread_mutex_t lock_buffer;

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

void *sync_receiver(void *_srv_descriptor)
{
    char buffer[1024];
    uint16_t data_size;
    int server_descriptor = *(int *)_srv_descriptor;

    while (1)
    {
        if (read(server_descriptor, buffer, 2) < 1)
            // die("Connection to the server was lost!");
            return NULL;

        // Received invalid data, do nothing
        if (!(buffer[0] == 'f' && buffer[1] == 's'))
            continue;

        // Read the next two bytes to determine the size of the packet
        if (read(server_descriptor, buffer, 2) < 1)
            die("Connection to the server was lost!");

        data_size = *(uint16_t *)buffer;

        // Read the remaining data
        if (read(server_descriptor, buffer, data_size - 2) < 1)
            die("Connection to the server was lost!");

        process_commands(buffer);
    }
}

void *sync_transmitter(void *_srv_descriptor)
{
    char buffer[1024];
    uint16_t data_size;
    int server_descriptor = *(int *)_srv_descriptor;

    while (1)
    {
        usleep(50000);
        data_size = read_data(buffer + 4, 1020);
        if (data_size == 0)
            continue;

        // Add 2 to content data to account for the data size field itself
        data_size += 2;

        // Write the frame start, content size and then data
        buffer[0] = 'f';
        buffer[1] = 's';
        buffer[2] = data_size & 255;
        buffer[3] = data_size >> 8;

        // Send data (add 2 for the frame start)
        write(server_descriptor, &buffer, data_size + 2);
    }
}

void process_commands(char *commands)
{
    rt_command function;
    function = commands[0];
    linestruct *tmp;
    // Lock the open file buffer from being modified
    pthread_mutex_lock(&lock_buffer);

    switch (function)
    {
    case APPEND_LINE:
        tmp = make_new_node(openfile->filebot);
        tmp->next = NULL;
        // Case of the first empty line and the server having a non empty first line
        if (openfile->filebot == openfile->filetop && strlen(openfile->filebot->data) == 0 && commands[1] != '\0')
        {
            tmp->lineno = 1;
            tmp->has_anchor = true;
            tmp->data = strdup(commands + 1);
            free(openfile->filebot->data);
            openfile->current = tmp;
            openfile->edittop = tmp;
            openfile->filetop = openfile->filebot = tmp;
        }
        else
        {
            tmp->lineno = openfile->filebot->lineno + 1;
            tmp->data = strdup(commands + 1);
            openfile->filebot->next = tmp;
            openfile->filebot = tmp;
        }
        break;
    case END_APPEND:
        download_done = true;
        break;
    default:
        return;
    }

    pthread_mutex_unlock(&lock_buffer);
}

// Starts the sync client: connects to the server and initiates transmit/receive threads
void start_client()
{
    pthread_attr_t thread_attr;
    pthread_mutexattr_t mtx_attr;
    pthread_t transmitter, receiver;
    pthread_mutexattr_init(&mtx_attr);
    pthread_mutex_init(&lock_buffer, &mtx_attr);
    pthread_attr_init(&thread_attr);

    // Create the socket to be used
    struct sockaddr_in out_socket;
    int server_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    out_socket.sin_addr.s_addr = inet_addr("127.0.0.1");
    out_socket.sin_port = htons(12000);
    out_socket.sin_family = AF_INET;

    if (connect(server_descriptor, (SA *)&out_socket, sizeof(out_socket)) == -1)
        die("Failed to connect to the target server...");

    // Start the sync client transmitter and receiver
    pthread_create(&transmitter, &thread_attr, sync_transmitter, &server_descriptor);
    pthread_create(&receiver, &thread_attr, sync_receiver, &server_descriptor);
    // pthread_join(transmitter, NULL);
    pthread_join(receiver, NULL);
    return;
}

int read_data(void *_dest, int N)
{
    int i;
    char *dest = _dest;
    while (pthread_mutex_trylock(&lock_buffer) != 0)
        ;

    for (i = 0; i < N && b_start != b_current; ++i)
    {
        dest[i] = tc_buffer[b_start++];
        // A circular buffer so it needs to wrap around
        if (b_start > TC_BUF_SIZE)
            b_start = 0;
    }
    pthread_mutex_unlock(&lock_buffer);
    return i;
}

int write_data(void *_src, int N)
{
    int i;
    char *source = _src;
    while (pthread_mutex_trylock(&lock_buffer) != 0)
        ;
    for (i = 0; i < N && b_start != b_current; ++i)
    {
        tc_buffer[b_current++] = source[i];
        // A circular buffer so it needs to wrap around
        if (b_current > TC_BUF_SIZE)
            b_current = 0;
    }
    pthread_mutex_unlock(&lock_buffer);
    return i;
}
