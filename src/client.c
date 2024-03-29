#include <config.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <sys/random.h>
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

// A high quality rand alternative
int32_t good_rand()
{
    static int32_t value;
    getrandom(&value, sizeof(value), 0);
    return value;
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
        else
        {
            process_commands(&p);
            free(p.data);
        }
    }
}

void *sync_transmitter(void *_srv_descriptor)
{
    int server_descriptor = *(int *)_srv_descriptor;
    payload p;

    while (1)
    {
        if (retrieve_packet(inter_thread_pipe[0], &p) == -1)
            die("Broken thread communication pipe!\n");
        else
        {
            send_packet(server_descriptor, &p);
            free(p.data);
        }
    }
}

void exec_add_user(payload *p)
{
    // If the user id is negative, the server is signaling that the absolute value is our ID
    if (p->user_id < 0)
        my_id = -p->user_id;
    else
    {
        clients[client_count].user_id = p->user_id;
        clients[client_count].current_line = NULL;
        ++client_count;
    }
}

void exec_remove_user(payload *p)
{
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
}

void exec_append_line(payload *p)
{
    linestruct *new_line;
    new_line = make_new_node(openfile->filebot);
    new_line->next = NULL;
    READ_BIN(new_line->id, p->data)
    new_line->data = strdup(p->data + 4);
    // Case of the first empty line and the server having a non empty first line
    if (openfile->filebot == openfile->filetop && strlen(openfile->filebot->data) == 0 && p->data[0] != '\0')
    {
        new_line->lineno = 1;
        new_line->has_anchor = true;

        new_line->prev = NULL;
        free(openfile->filebot->data);
        openfile->current = new_line;
        openfile->edittop = new_line;
        openfile->filetop = openfile->filebot = new_line;
    }
    else
    {
        new_line->lineno = openfile->filebot->lineno + 1;
        openfile->filebot->next = new_line;
        openfile->filebot = new_line;
    }
}

void exec_add_line(payload *p)
{
    linestruct *target, *newline;
    int32_t after_id, with_id;
    READ_BIN(after_id, p->data)
    READ_BIN(with_id, p->data + 4)
    target = find_line_by_id(after_id);
    newline = make_new_node(target);
    // Relink the target
    newline->next = target->next;
    newline->prev = target;
    if (newline->next != NULL)
            newline->next->prev = newline;
    target->next = newline;
    newline->id = with_id;
    // If data size is 8, no string to initialize the line
    if (p->data_size == 8)
        newline->data = strdup("");
    else
    {
        newline->data = malloc(p->data_size - 7);
        newline->data = strncpy(newline->data, p->data + 8, p->data_size - 8);
        newline->data[p->data_size - 8] = '\0';
    }
    renumber_from(target);
}

void exec_replace_line(payload *p)
{
    linestruct *target;
    int32_t target_id;
    READ_BIN(target_id, p->data)
    target = find_line_by_id(target_id);
    free(target->data);
    target->data = malloc(p->data_size - 3);
    strncpy(target->data, p->data + 4, p->data_size - 4);
    target->data[p->data_size - 4] = '\0';
    // In case the cursor went out of bound of the replacement line, set it at the last position
    if (openfile->current == target && openfile->current_x > strlen(target->data))
        openfile->current_x = strlen(target->data) - 1;
}

void exec_add_string(payload *p)
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
    // Update my cursor if necessary
    if (openfile->current == target)
        openfile->current_x += puddle_len;
}

void exec_remove_string(payload *p)
{
    linestruct *target;
    int32_t target_id, column, count;
    READ_BIN(target_id, p->data)
    READ_BIN(column, p->data + 4)
    READ_BIN(count, p->data + 8)
    target = find_line_by_id(target_id);
    // Remove line break and merge with previous line
    if (column == -1)
    {
        linestruct *prev = target->prev;
        if (prev == NULL)
            return;
        // If the amount to delete is more than 1, we need to remove count-1 characters from the beginning
        if (count > 1)
            memmove(target->data, target->data + count - 1, strlen(target->data + count - 1) + 1);

        // Move the cursor to the previous line and to the correct place
        if (openfile->current == target)
        {
            openfile->current_x = openfile->current_x - (count - 1) + strlen(prev->data);
            openfile->current = prev;
        }
        prev->data = realloc(prev->data, strlen(prev->data) + strlen(target->data) + 1);
        strcat(prev->data, target->data);
        unlink_node(target);
        renumber_from(prev);
    }
    // Remove line break and merge with next line
    else if (column + count > strlen(target->data))
    {
        linestruct *next = target->next;
        if (next == NULL)
            return;

        // Move the cursor to the current line since its line is gone
        if (openfile->current == next)
        {
            openfile->current_x = openfile->current_x - (count - 1) + strlen(target->data);
            openfile->current = target;
        }

        target->data = realloc(target->data, strlen(target->data) + strlen(next->data) + 1);
        strcat(target->data, next->data);
        --count;
        memmove(target->data + column, target->data + column + count, strlen(target->data + column + count) + 1);
        unlink_node(next);
        renumber_from(target);
    }
    else
    {
        // Remove the characters by moving the remaining part of the string
        memmove(target->data + column, target->data + column + count, strlen(target->data) - column - count + 1);
        target->data = nrealloc(target->data, strlen(target->data) + 1);
        // Update my cursor if necessary
        if (openfile->current == target && column < openfile->current_x)
            openfile->current_x = openfile->current_x - count;
    }
}

void exec_move_cursor(payload *p)
{
    int i;
    for (i = 0; i < client_count; ++i)
        if (p->user_id == clients[i].user_id)
            break;

    int32_t id;
    READ_BIN(id, p->data)
    clients[i].current_line = find_line_by_id(id);
    READ_BIN(clients[i].xpos, p->data + 4)
}

void process_commands(payload *p)
{
    // Lock the open file buffer to block the main thread from editing it
    pthread_mutex_lock(&lock_openfile);

    switch (p->function)
    {
    case ADD_USER:
        exec_add_user(p);
        break;

    case REMOVE_USER:
        exec_remove_user(p);
        break;

    case APPEND_LINE:
        exec_append_line(p);
        break;

    case END_APPEND:
        download_done = true;
        break;

    case ADD_LINE:
        exec_add_line(p);
        break;

    case REPLACE_LINE:
        exec_replace_line(p);
        break;

    case ADD_STR:
        exec_add_string(p);
        break;

    case REMOVE_STR:
        exec_remove_string(p);
        break;

    case MOVE_CURSOR:
        exec_move_cursor(p);
        break;
    }

    if (download_done)
    {
        edit_refresh();
        doupdate();
    }

    pthread_mutex_unlock(&lock_openfile);
}

// Starts the sync client: connects to the server and initiates transmit/receive threads
void start_client()
{
    pthread_attr_t thread_attr;
    pthread_mutexattr_t mtx_attr;
    pthread_t transmitter, receiver, cursor_monitor;
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
    pthread_create(&cursor_monitor, &thread_attr, report_cursor_move, NULL);
    pthread_detach(transmitter);
    pthread_detach(receiver);
    pthread_detach(cursor_monitor);

    // Preprare color pair for the remote user cursors
    init_pair(127, COLOR_BLACK, COLOR_GREEN);
    return;
}

// Same as read(), but doesn't return unless n bytes are read (or an error occured)
int read_n(int fd, void *b, size_t n)
{
    int last, total;
    total = 0;
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

// Loop that polls cursor movement and sends updates up to 20 times/second
void *report_cursor_move(void *nothing)
{
    char data[8];
    int32_t prev_id, prev_x;
    payload p;
    p.function = MOVE_CURSOR;
    p.data = data;
    p.data_size = sizeof(data);

    while (1)
    {
        if (openfile->current->id != prev_id || openfile->current_x != prev_x)
        {
            prev_id = openfile->current->id;
            WRITE_BIN(prev_id, data)
            prev_x = openfile->current_x;
            WRITE_BIN(prev_x, data + 4)
            send_packet(inter_thread_pipe[1], &p);
        }
        usleep(50000);
    }
}

void report_insertion(char *burst)
{
    payload p;
    p.function = ADD_STR;
    p.data_size = 8 + strlen(burst);
    char buffer[p.data_size];
    p.data = buffer;
    WRITE_BIN(openfile->current->id, p.data)
    // This is to cast the value to a 4 byte variable and use it in the macro
    int32_t x = openfile->current_x;
    WRITE_BIN(x, p.data + 4)
    strncpy(p.data + 8, burst, strlen(burst));
    send_packet(inter_thread_pipe[1], &p);
}

void report_deletion(bool is_backspace)
{
    payload p;
    p.function = REMOVE_STR;
    p.data_size = 12;
    char buffer[p.data_size];
    p.data = buffer;
    WRITE_BIN(openfile->current->id, p.data)
    int32_t tmp = openfile->current_x;

    if (is_backspace)
        --tmp;

    WRITE_BIN(tmp, p.data + 4)
    tmp = 1;
    WRITE_BIN(tmp, p.data + 8)
    send_packet(inter_thread_pipe[1], &p);
}

void report_enter()
{
    // Since a line broke into two and maybe nano was configured to auto-indent
    // The easiest way is to tell the server what the old line became and the content of the new line
    payload p;
    p.function = REPLACE_LINE;
    p.data_size = 4 + strlen(openfile->current->prev->data);
    char original_line[p.data_size];
    strncpy(original_line + 4, openfile->current->prev->data, p.data_size - 4);
    p.data = original_line;
    WRITE_BIN(openfile->current->prev->id, p.data)
    // Send the line replacement command
    send_packet(inter_thread_pipe[1], &p);

    // Now send the new line
    p.function = ADD_LINE;
    p.data_size = 8 + strlen(openfile->current->data);
    char new_line[p.data_size];
    p.data = new_line;
    WRITE_BIN(openfile->current->prev->id, p.data)
    openfile->current->id = good_rand();
    WRITE_BIN(openfile->current->id, p.data + 4)
    strncpy(new_line + 8, openfile->current->data, p.data_size - 8);
    // Send the new line
    send_packet(inter_thread_pipe[1], &p);
}
