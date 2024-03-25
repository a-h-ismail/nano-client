#include <config.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include "client.h"
#include "prototypes.h"

int b_start;
int b_current;
bool remote_buffer = false;
bool download_done = false;
int8_t my_id;

// [0] for read, [1] for write
int inter_thread_pipe[2];
client_data clients[9];
int client_count;

void process_commands(payload *p);
pthread_mutex_t lock_openfile, lock_tc;

linestruct *find_line_by_id(int32_t id)
{
    linestruct *match = openfile->filetop;
    while (match != NULL)
    {
        if (match->id == id)
            break;
        else
            match = match->next;
    }
    return match;
}

int send_packet(int descriptor, payload *p)
{
    char send_buffer[1024];
    // +2 for the function and user id
    uint16_t payload_size = p->data_size + 2;
    // Frame start
    send_buffer[0] = '\a';
    // Payload size is bytes 1-2
    WRITE_BIN(payload_size, send_buffer + 1);

    send_buffer[3] = my_id;
    // Function
    send_buffer[4] = p->function;
    // Data
    memcpy(send_buffer + 5, p->data, p->data_size);
    // +3 for the frame start and payload size
    return write(descriptor, send_buffer, payload_size + 3);
}

int retrieve_packet(int descriptor, payload *p)
{
    uint16_t size;
    char recv_buffer[1024];
    if (read(descriptor, recv_buffer, 1) < 1)
        return -1;

    // Check for the frame start
    if (recv_buffer[0] != '\a')
        return -1;

    if (read_n(descriptor, recv_buffer, 2) < 1)
        return -1;

    READ_BIN(size, recv_buffer);

    // Read the user_id, function and its data
    if (read_n(descriptor, recv_buffer, size) < 1)
        return -1;

    p->user_id = recv_buffer[0];
    p->function = (rt_command)recv_buffer[1];
    p->data = malloc(size - 2);
    memcpy(p->data, recv_buffer + 2, size - 2);
    p->data_size = size - 2;
    return 0;
}

void *sync_receiver(void *_srv_descriptor)
{
    int server_descriptor = *(int *)_srv_descriptor;
    payload p;

    while (1)
    {
        if (retrieve_packet(server_descriptor, &p) == -1)
            die("Connection to the server was lost!");

        process_commands(&p);
    }
}

void *sync_transmitter(void *_srv_descriptor)
{
    int server_descriptor = *(int *)_srv_descriptor;
    payload p;

    while (1)
    {
        retrieve_packet(inter_thread_pipe[0], &p);
        send_packet(server_descriptor, &p);
    }
}

void process_commands(payload *p)
{
    rt_command function;
    function = p->function;
    linestruct *tmp;
    size_t x_y[2];
    // Lock the open file buffer from being modified
    pthread_mutex_lock(&lock_openfile);

    switch (function)
    {
    case ADD_USER:
        // If the user id is negative, the server is signaling that the absolute value is our ID
        if (p->user_id < 0)
            my_id = -p->user_id;
        else
        {
            clients[client_count].user_id = p->user_id;
            clients[client_count].current_line = NULL;
            ++client_count;
        }
        break;

    case REMOVE_USER:
        for (int i = 0; i < client_count; ++i)
        {
            if (clients[i].user_id == p->user_id)
            {
                free(clients[i].name);
                // Remove the client entry
                memmove(clients + i, clients + i + 1, sizeof(clients) * (client_count - i - 1));
                --client_count;
                break;
            }
        }
        break;

    case APPEND_LINE:
        tmp = make_new_node(openfile->filebot);
        tmp->next = NULL;
        READ_BIN(tmp->id, p->data)
        tmp->data = strdup(p->data + 4);
        // Case of the first empty line and the server having a non empty first line
        if (openfile->filebot == openfile->filetop && strlen(openfile->filebot->data) == 0 && p->data[0] != '\0')
        {
            tmp->lineno = 1;
            tmp->has_anchor = true;

            tmp->prev = NULL;
            free(openfile->filebot->data);
            openfile->current = tmp;
            openfile->edittop = tmp;
            openfile->filetop = openfile->filebot = tmp;
        }
        else
        {
            tmp->lineno = openfile->filebot->lineno + 1;
            tmp->data = strdup(p->data + 4);
            openfile->filebot->next = tmp;
            openfile->filebot = tmp;
        }
        break;
    case END_APPEND:
        download_done = true;
        break;

    case ADD_STR:
    {
        linestruct *target;
        int32_t target_id, column, puddle_len = p->data_size - 8;
        READ_BIN(target_id, p->data)
        READ_BIN(column, p->data + 4)
        target = find_line_by_id(target_id);
        // Make room for the substring
        target->data = nrealloc(target->data, strlen(target->data) + puddle_len + 1);
        memmove(target->data + column + puddle_len, target->data + column, strlen(target->data) - column - puddle_len + 2);
        memcpy(target->data + column, p->data + 8, puddle_len);
    }
    break;

    case REMOVE_STR:
    {
        linestruct *target;
        int32_t target_id, column, count;
        READ_BIN(target_id, p->data)
        READ_BIN(column, p->data + 4)
        READ_BIN(count, p->data + 8)
        target = find_line_by_id(target_id);
        // Remove the characters by moving the remaining part of the string
        memmove(target->data + column, target->data + column + count, strlen(target->data) - column - count + 1);
        target->data = nrealloc(target->data, strlen(target->data) + 1);
    }
    break;

    case MOVE_CURSOR:
        READ_BIN(x_y, p->data);
    }

    if (download_done)
        draw_all_subwindows();

    free(p->data);
    pthread_mutex_unlock(&lock_openfile);
}

// Starts the sync client: connects to the server and initiates transmit/receive threads
void start_client()
{
    pthread_attr_t thread_attr;
    pthread_mutexattr_t mtx_attr;
    pthread_t transmitter, receiver;
    pthread_mutexattr_init(&mtx_attr);
    pthread_mutex_init(&lock_openfile, &mtx_attr);
    pthread_mutex_init(&lock_tc, &mtx_attr);
    pthread_attr_init(&thread_attr);

    // Create the socket to be used
    struct sockaddr_in out_socket;
    int server_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    out_socket.sin_addr.s_addr = inet_addr("127.0.0.1");
    out_socket.sin_port = htons(12000);
    out_socket.sin_family = AF_INET;

    if (connect(server_descriptor, (SA *)&out_socket, sizeof(out_socket)) == -1)
        die("Failed to connect to the target server...");

    // Create the pipe used to communicate between threads
    if (pipe(inter_thread_pipe) == -1)
        die("Failed to initialize inter thread communication!");

    // Start the sync client transmitter and receiver
    pthread_create(&transmitter, &thread_attr, sync_transmitter, &server_descriptor);
    pthread_create(&receiver, &thread_attr, sync_receiver, &server_descriptor);
    pthread_detach(transmitter);
    pthread_detach(receiver);
    return;
}

// Same as read(), but doesn't return unless n bytes are read (or an error occured)
int read_n(int fd, void *b, size_t n)
{
    int last, total;
    last = total = 0;
    while (total < n)
    {
        last = read(fd, (char *)b + total, n);
        if (last < 1)
            return last;
        else
            total += last;
    }
    return total;
}

// Reports the current cursor position, call after any cursor movement
void report_cursor_move()
{
    char data[sizeof(size_t) * 2];
    WRITE_BIN(openfile->current_x, data);
    WRITE_BIN(openfile->current_y, data + sizeof(size_t));

    payload p;
    p.function = MOVE_CURSOR;
    p.data = data;
    p.data_size = 16;

    send_packet(inter_thread_pipe[1], &p);
}

void report_insertion(char *burst)
{
    payload p;
    p.function = ADD_STR;
    p.data_size = 8 + strlen(burst);
    p.data = malloc(p.data_size);
    WRITE_BIN(openfile->current->id, p.data)
    // This is to cast the value to a 4 byte variable and use it in the macro
    int32_t x = openfile->current_x;
    WRITE_BIN(x, p.data + 4)
    strncpy(p.data + 8, burst, strlen(burst));
    send_packet(inter_thread_pipe[1], &p);
}

void report_deletion()
{
    payload p;
    p.function = REMOVE_STR;
    p.data_size = 12;
    p.data = malloc(p.data_size);
    WRITE_BIN(openfile->current->id, p.data)
    // This is to cast the value to a 4 byte variable and use it in the macro
    int32_t tmp = openfile->current_x;

    WRITE_BIN(tmp, p.data + 4)
    tmp = 1;
    WRITE_BIN(tmp, p.data + 8)
    send_packet(inter_thread_pipe[1], &p);
}