#include <config.h>
#include <assert.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <sys/random.h>
#include "client.h"
#include "prototypes.h"

bool remote_buffer = false;
bool download_done = false;
int8_t my_id;
int server_fd;
char *server_ip;
char *alternate_title = NULL;
char *remote_filename = NULL;

client_data clients[9];
int client_count;

void process_commands(payload *p);
pthread_mutex_t lock_openfile, lock_tc;

void update_remote_title()
{
    free(alternate_title);
    asprintf(&alternate_title, "file://%s/%s   Online: %d", server_ip, remote_filename, client_count + 1);
    titlebar(NULL);
}

// Insert a node after the specified node
// Does not set the ID
linestruct *insert_node_after(linestruct *prev)
{
    if (prev == NULL)
        return NULL;

    linestruct *newnode = nmalloc(sizeof(linestruct));
    newnode->prev = prev;
    newnode->next = prev->next;
    prev->next = newnode;
    if (newnode->next != NULL)
        newnode->next->prev = newnode;
    renumber_from(prev);
    newnode->has_anchor = false;
    newnode->is_locked = false;
    newnode->data = NULL;
    newnode->multidata = NULL;
    return newnode;
}

bool function_remote_compatible(void *f)
{
    void *allowed[] = {
        do_page_up,
        do_page_down,
        do_enter,
        do_up,
        do_down,
        do_left,
        do_right,
        do_backspace,
        do_delete,
        do_home,
        do_end,
        do_center,
        do_exit,
        do_tab,
        NULL};
    for (int i = 0; i < sizeof(allowed) / sizeof(*allowed); ++i)
        if (allowed[i] == f)
            return true;

    return false;
}

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

int send_payload(int fd, payload *p)
{
    assert(p->data_size <= DATA_MAX);

    char send_buffer[DATA_MAX];
    uint16_t payload_size = p->data_size + PREAMBLE_SIZE;
    // Preamble section: payload start, data size, user ID and function
    send_buffer[0] = '\a';
    WRITE_BIN(p->data_size, send_buffer + 1);
    send_buffer[3] = my_id;
    send_buffer[4] = p->function;
    // Data section
    memcpy(send_buffer + 5, p->data, p->data_size);
    return write(fd, send_buffer, payload_size);
}

int retrieve_payload(int fd, payload *p)
{
    uint16_t dsize;
    char recv_buffer[DATA_MAX];

    // Read the preamble section
    if (read_n(fd, recv_buffer, PREAMBLE_SIZE) < 1)
        return -1;
    if (recv_buffer[0] != '\a')
        return -1;
    READ_BIN(dsize, recv_buffer + 1);
    if (dsize > DATA_MAX)
        return -1;
    p->data_size = dsize;
    p->user_id = recv_buffer[3];
    p->function = recv_buffer[4];

    // Read the data section
    if (dsize > 0 && read_n(fd, recv_buffer, dsize) < 1)
        return -1;

    memcpy(p->data, recv_buffer, dsize);
    p->data_size = dsize;
    return 0;
}

void *sync_receiver(void *_srv_descriptor)
{
    int server_descriptor = *(int *)_srv_descriptor;
    payload p;

    while (1)
    {
        if (retrieve_payload(server_descriptor, &p) == -1)
            die("Connection to the server was lost!\n");
        else
            process_commands(&p);
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
    update_remote_title();
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
    update_remote_title();
}

void exec_append_line(payload *p)
{
    linestruct *newline;
    newline = make_new_node(openfile->filebot);
    newline->next = NULL;
    READ_BIN(newline->id, p->data);
    newline->data = strdup(p->data + 4);
    // Case of the first empty line and the server having a non empty first line
    if (openfile->filebot == openfile->filetop && strlen(openfile->filebot->data) == 0 && p->data[0] != '\0')
    {
        newline->lineno = 1;
        newline->has_anchor = true;

        newline->prev = NULL;
        free(openfile->filebot->data);
        openfile->current = newline;
        openfile->edittop = newline;
        openfile->filetop = openfile->filebot = newline;
    }
    else
    {
        newline->lineno = openfile->filebot->lineno + 1;
        openfile->filebot->next = newline;
        openfile->filebot = newline;
    }
}

void exec_add_line(payload *p)
{
    linestruct *target, *newline;
    int32_t after_id, with_id;
    READ_BIN(after_id, p->data);
    READ_BIN(with_id, p->data + 4);
    target = find_line_by_id(after_id);
    if (target == NULL)
        return;
    newline = insert_node_after(target);
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

void exec_remove_line(payload *p)
{
    linestruct *target;
    int32_t target_id;
    READ_BIN(target_id, p->data);
    target = find_line_by_id(target_id);
    if (target != NULL)
        unlink_node(target);
}

void exec_replace_line(payload *p)
{
    linestruct *target;
    int32_t target_id;
    READ_BIN(target_id, p->data);
    target = find_line_by_id(target_id);
    if (target == NULL)
        return;
    target->data = realloc(target->data, p->data_size - 3);
    strncpy(target->data, p->data + 4, p->data_size - 4);
    target->data[p->data_size - 4] = '\0';
    // In case the cursor went out of bound of the replacement line, set it at the last position
    if (openfile->current == target && openfile->current_x > strlen(target->data))
        openfile->current_x = strlen(target->data) - 1;
}

void exec_break_line(payload *p)
{
    linestruct *target, *newline;
    int32_t target_id, newline_id, column, prefix_len = p->data_size - 12;
    READ_BIN(target_id, p->data);
    READ_BIN(column, p->data + 4);
    READ_BIN(newline_id, p->data + 8);

    target = find_line_by_id(target_id);
    if (target == NULL)
        return;
    newline = insert_node_after(target);
    newline->id = newline_id;
    // The next line will have the prefix and the content at the line breaking position
    newline->data = nmalloc(prefix_len + strlen(target->data) - column + 1);
    if (prefix_len != 0)
        strncpy(newline->data, p->data + 12, prefix_len);

    strcpy(newline->data, target->data + column);
    target->data = nrealloc(target->data, column + 1);
    target->data[column] = '\0';

    // Move my cursor if the line broke before its position
    if (openfile->current == target && openfile->current_x >= column)
    {
        openfile->current = newline;
        openfile->current_x = prefix_len + openfile->current_x - column;
        ++openfile->current_y;
    }
}

void exec_add_string(payload *p)
{
    linestruct *target;
    int32_t target_id, column, puddle_len = p->data_size - 8;
    READ_BIN(target_id, p->data);
    READ_BIN(column, p->data + 4);
    target = find_line_by_id(target_id);
    if (target == NULL)
        return;
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
    READ_BIN(target_id, p->data);
    READ_BIN(column, p->data + 4);
    READ_BIN(count, p->data + 8);
    target = find_line_by_id(target_id);
    if (target == NULL)
        return;
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
    READ_BIN(id, p->data);
    clients[i].current_line = find_line_by_id(id);
    READ_BIN(clients[i].xpos, p->data + 4);
}

void handle_status(payload *p)
{
    switch (p->data[0])
    {
    case ACCEPTED:
        break;
    case FILE_INACCESSIBLE:
        die("The requested file is inaccessible.\n");
    case CLIENTS_EXCEEDED:
        die("Client count for this file exceeded on the server!\n");
    case PROTOCOL_ERROR:
        die("A Protocol error occured, possibly a client/server mismatch.");
    }
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
        update_remote_title();
        break;

    case BREAK_LINE:
        exec_break_line(p);
        break;

    case ADD_LINE:
        exec_add_line(p);
        break;

    case REMOVE_LINE:
        exec_remove_line(p);
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

// Starts the sync client: connects to the server, requests a file and initiates transmit/receive threads
void start_client()
{
    pthread_attr_t thread_attr;
    pthread_mutexattr_t mtx_attr;
    pthread_t receiver, cursor_monitor;
    pthread_mutexattr_init(&mtx_attr);
    pthread_mutex_init(&lock_openfile, &mtx_attr);
    pthread_mutex_init(&lock_tc, &mtx_attr);
    pthread_attr_init(&thread_attr);

    // Create the socket to be used
    struct sockaddr_in out_socket;
    int server_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    out_socket.sin_addr.s_addr = inet_addr(server_ip);
    out_socket.sin_port = htons(12000);
    out_socket.sin_family = AF_INET;

    if (connect(server_descriptor, (SA *)&out_socket, sizeof(out_socket)) == -1)
        die("Failed to connect to the target server...\n");
    else
        server_fd = server_descriptor;

    // Request the filename to open
    payload request_file;
    request_file.function = OPEN_FILE;
    request_file.data_size = strlen(remote_filename);
    strncpy(request_file.data, remote_filename, request_file.data_size);
    send_payload(server_descriptor, &request_file);

    // Check the response code
    payload response;
    retrieve_payload(server_descriptor, &response);
    handle_status(&response);

    // Start the sync client transmitter and receiver
    pthread_create(&receiver, &thread_attr, sync_receiver, &server_descriptor);
    pthread_create(&cursor_monitor, &thread_attr, report_cursor_move, NULL);
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
        last = read(fd, (char *)b + total, n - total);
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
    int32_t prev_id = openfile->current->id, prev_x = openfile->current_x;
    payload p;
    p.function = MOVE_CURSOR;
    p.data_size = 8;

    while (1)
    {
        if (openfile->current->id != prev_id || openfile->current_x != prev_x)
        {
            prev_id = openfile->current->id;
            WRITE_BIN(prev_id, p.data);
            prev_x = openfile->current_x;
            WRITE_BIN(prev_x, p.data + 4);
            send_payload(server_fd, &p);
        }
        usleep(50000);
    }
}

void draw_remote_cursors()
{
    for (int i = 0; i < client_count; ++i)
        if (clients[i].current_line != NULL)
        {
            size_t calculated_y = clients[i].current_line->lineno - openfile->edittop->lineno;
            size_t calculated_x = wideness(clients[i].current_line->data, clients[i].xpos);

            // When the line is longer than the window width, we should calculate the relative remote cursor position.
            if (clients[i].current_line == openfile->current && openfile->current_x > editwincols)
            {
                size_t xpage_start = get_page_start(calculated_x);
                size_t cursor_page_start = get_page_start(openfile->current_x);
                if (xpage_start == cursor_page_start)
                    calculated_x -= xpage_start;
            }

            if (calculated_y >= 0 && calculated_y < editwinrows)
            {
                mvwchgat(midwin, calculated_y, calculated_x, 1, A_COLOR, 127, NULL);
            }
        }
    wnoutrefresh(midwin);
    doupdate();
}

void report_insertion(char *burst)
{
    payload p;
    p.function = ADD_STR;
    p.data_size = 8 + strlen(burst);
    WRITE_BIN(openfile->current->id, p.data);
    // This is to cast the value to a 4 byte variable and use it in the macro
    int32_t x = openfile->current_x;
    WRITE_BIN(x, p.data + 4);
    strncpy(p.data + 8, burst, strlen(burst));
    send_payload(server_fd, &p);
}

void report_deletion(bool is_backspace)
{
    payload p;
    p.function = REMOVE_STR;
    p.data_size = 12;
    WRITE_BIN(openfile->current->id, p.data);
    int32_t tmp = openfile->current_x;

    if (is_backspace)
        --tmp;

    WRITE_BIN(tmp, p.data + 4);
    tmp = 1;
    WRITE_BIN(tmp, p.data + 8);
    send_payload(server_fd, &p);
}

void report_enter(bool first_call)
{
    static int prev_x;
    payload p;
    if (first_call)
    {
        prev_x = openfile->current_x;
        return;
    }
    else
    {
        linestruct *prev = openfile->current->prev;
        openfile->current->id = good_rand();
        // When you break a line, the text editor may add indentation at the beginning of the line
        // So check if the cursor is at the beginning of the line or not
        if (openfile->current_x > 0)
            strncpy(p.data + 12, prev->data, strlen(prev->data) - prev_x);

        // Format: line_to_break | pos_in_line | new_line | prefix
        p.function = BREAK_LINE;
        p.data_size = 12 + strlen(prev->data) - prev_x;
        WRITE_BIN(prev->id, p.data);
        WRITE_BIN(prev_x, p.data + 4);
        WRITE_BIN(openfile->current->id, p.data + 8);
        send_payload(server_fd, &p);
    }
}
