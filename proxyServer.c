#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "threadpool.h"
#define IPV4_BINARY_LENGTH 32
#define READ_BUFFER_LEN 1000
#define RESPONSE_BUFFER_LEN 5000

typedef struct Client{
    struct sockaddr_in sock_info;
    int sock_fd;
}Client;

struct Node {
    char *line;
    struct Node *next;
};



int client_handler (void*);
int check_request(const char *);
void arguments_check(const int *,const size_t *,const size_t*);
void insert_host(char *);
int search_host(char *);
char* to_binary(char*);
char* token_to_binary(int);
char* error_generator(int);
void write_to_socket(int, char*, unsigned long);
void free_list();
void write_to_client_socket(int, unsigned char*, unsigned long);

struct Node* filter_head = NULL;
int main(int argc, char* argv[]) {
    if (argc != 5) {
        printf("Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        exit(EXIT_FAILURE);
    }
    //read the arguments from the command line, convert them to specified types
    int port_i = (in_port_t)strtoul(argv[1],NULL, 10);
    size_t pool_size = strtoul(argv[2], NULL, 10);
    size_t max_tasks = strtoul(argv[3], NULL, 10);
    char* file_path = argv[4];

    arguments_check(&port_i, &pool_size, &max_tasks);
    in_port_t port = (in_port_t)port_i;
    // open the filter file
    FILE *file = fopen(file_path, "r");
    if (file == NULL) {
        printf("Error opening file\n");
        exit(EXIT_FAILURE);
    }
    // create a linked list with names and ips of forbidden hosts
    char file_read_buffer[256];
    while (fgets(file_read_buffer, sizeof(file_read_buffer), file) != NULL) {
        // Remove newline character from the end of the line
        file_read_buffer[strcspn(file_read_buffer, "\r\n")] = '\0';
        insert_host(file_read_buffer);
    }
    fclose(file);
    file = NULL;

    // create our proxy server
    struct sockaddr_in* proxy_info = (struct sockaddr_in*)malloc(sizeof (struct sockaddr_in));
    memset(proxy_info, 0, sizeof(struct sockaddr_in));
    proxy_info ->sin_family = AF_INET;
    proxy_info -> sin_port = htons(port);
    proxy_info -> sin_addr.s_addr = htonl(INADDR_ANY);

    int welcome_socket;
    if((welcome_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1){
        perror("socket");
        free_list();
        exit(EXIT_FAILURE);
    }

    if(bind(welcome_socket, (struct sockaddr*)proxy_info, sizeof (struct sockaddr_in)) == -1){
        close(welcome_socket);
        free(proxy_info);
        free_list();
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if(listen(welcome_socket, 5) == -1){
        close(welcome_socket);
        free(proxy_info);
        free_list();
        perror("listen");
        exit(EXIT_FAILURE);
    }
    threadpool* tp = create_threadpool((int)pool_size);
    // using a loop accept requests, limited by max num of requests
    for (size_t i = 0; i < max_tasks; ++i) {
        Client* client = (Client*) malloc(sizeof(Client));
        socklen_t struct_len = sizeof(struct sockaddr_in);
        client -> sock_fd = accept(welcome_socket, (struct sockaddr*)&client->sock_info,
                                   &struct_len);
        dispatch(tp, client_handler, (void *) client);
    }

    // free our resources
    destroy_threadpool(tp);
    close(welcome_socket);
    free(proxy_info);
    free_list();
    return 0;
}

int client_handler(void* arg) {
    Client *client = (Client *) arg;
    char read_buffer[READ_BUFFER_LEN];
    memset(read_buffer, 0, READ_BUFFER_LEN);
    char *client_request = (char *) malloc(READ_BUFFER_LEN);
    size_t allocated_size = READ_BUFFER_LEN;
    memset(client_request, 0, allocated_size);
    size_t bytes_read;
    size_t total_bytes_read = 0;
    do{ // read until we find the end of request signature
        bytes_read = read(client->sock_fd, read_buffer, sizeof(read_buffer));
        if (bytes_read <= 0) {
            break;
        }
        total_bytes_read += bytes_read; // accumulate the total response length
        if (total_bytes_read >= allocated_size) { // Check the need of reallocation
            allocated_size *= 2; // Double the allocated size
            char *temp = realloc(client_request, allocated_size);
            if (temp == NULL) {
                char* msg = error_generator(500);
                write_to_socket(client -> sock_fd, msg, strlen(msg));
                free(msg);
                free(client_request);
                return 0;
            }
            client_request = temp;
        }
        memcpy(client_request + total_bytes_read - bytes_read, read_buffer, bytes_read);
        memset(read_buffer, 0, allocated_size);

    //+ total_bytes_read - 4
    } while (strstr((char *) client_request, "\r\n\r\n") == NULL);
    if(total_bytes_read == 0){
        return 0;
    }
    client_request[total_bytes_read] = '\0';
    int validity;
    int sock_fd = -1;
    //check if the three tokens exist
    if ((validity = check_request((char *) client_request)) != 1){
        char* msg =  error_generator(validity);
        write_to_socket(client -> sock_fd, msg, strlen(msg));
        free(msg);
        goto termination;
    }

    //extract the host name and the port(if existed) from the client request
    char host[100];
    char* host_start = strstr(client_request, "Host: ") + strlen("Host: ");
    char* host_end = strstr(host_start, ":");
    host_end = (host_end != NULL && host_end < strstr(host_start, "\r\n")) ? host_end : strstr(host_start, "\r\n");
    int i = 0;
    for (; i < host_end - host_start; i++) {
        host[i] = *(host_start + i);
    }
    host[i] = '\0';


    in_port_t port;
    char* port_start = *host_end != ':' ? NULL : host_end + 1;
    if(port_start != NULL){
        char * port_end = strstr(port_start, "\r\n");
        char port_str[10];
        memset(port_str, 0, 10);
        i = 0;
        for (; i < port_end - port_start; i++) {
            port_str[i] = *(port_start + i);
        }
        port_str[i] = '\0';
        port = strtoul(port_str, NULL, 10);
    }
    else{
        port = 80;
    }
    // get the server info to connect it
    struct hostent *server_info = NULL;
    server_info = gethostbyname(host);
    struct sockaddr_in sock_info;

    if(server_info != NULL) {
        memset(&sock_info, 0, sizeof(struct sockaddr_in));
        sock_info.sin_family = AF_INET;
        sock_info.sin_port = htons(port);
        sock_info.sin_addr.s_addr = ((struct in_addr *) server_info->h_addr_list[0])->s_addr;
    }
    else{
        char* msg = error_generator(404);
        write_to_socket(client -> sock_fd, msg, strlen(msg));
        free(msg);
        goto termination;

    }
    if(search_host(host)){
        char* msg = error_generator(403);
        write_to_socket(client -> sock_fd, msg, strlen(msg));
        free(msg);
        goto termination;
    }

    if ((sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
        char* msg = error_generator(500);
        write_to_socket(client -> sock_fd, msg, strlen(msg));
        free(msg);
        goto termination;
    }
    if (connect(sock_fd, (struct sockaddr *) &sock_info, sizeof(struct sockaddr_in)) == -1) {
        char* msg = error_generator(500);
        write_to_socket(client -> sock_fd, msg, strlen(msg));
        free(msg);
        goto termination;

    }

    //change the connection attribute to close, or add it.
    char* connection_start = strstr(client_request, "Connection: ");
    if (connection_start != NULL) {
        // Calculate the length of the substring before "Connection: "
        size_t prefix_length = connection_start - client_request;

        // Allocate memory for the new request, including additional space for "Connection: close"
        char* new_request = (char*)malloc(prefix_length + strlen("Connection: close\r\n\r\n") + 1);
        memcpy(new_request, client_request, prefix_length);
        strcpy(new_request + prefix_length, "Connection: close\r\n\r\n");
        free(client_request);
        client_request = new_request;
    } else {
        char* new_request = (char*)malloc(strlen(client_request) - 4 + strlen("Connection: close\r\n\r\n") + 1);
        strcpy(new_request, client_request);
        memcpy(new_request + strlen(client_request) - 4, "Connection: close\r\n\r\n", strlen("Connection: close\r\n\r\n"));
        free(client_request);
        client_request = new_request;
    }

    if (send(sock_fd, client_request, strlen(client_request), 0) == -1) {
        char* msg = error_generator(500);
        write_to_socket(client -> sock_fd, msg, strlen(msg));
        free(msg);
        goto termination;
    }

    unsigned char response_buffer[RESPONSE_BUFFER_LEN];
    memset(response_buffer, 0, RESPONSE_BUFFER_LEN);
    ssize_t response_bytes_read = read(sock_fd, response_buffer, RESPONSE_BUFFER_LEN);
    while(response_bytes_read > 0){
        write_to_client_socket(client -> sock_fd, response_buffer, response_bytes_read);
        memset(response_buffer, 0, RESPONSE_BUFFER_LEN);
        response_bytes_read = read(sock_fd, response_buffer, RESPONSE_BUFFER_LEN);
    }
    termination:
    if(sock_fd != -1) {
        close(sock_fd);
    }
    close(client -> sock_fd);
    free(client_request);
    return 1;
}



int check_request(const char * request) {
    char* first_row_end = strstr(request, "\r\n");
    if(!first_row_end || first_row_end < request){
        return 400;
    }
    ssize_t first_row_len = first_row_end - request;//copy the first len to check essential tokens
    char* first_row = (char*) malloc(first_row_len + 1);
    memset(first_row, 0, first_row_len);
    memcpy(first_row, request, first_row_len);
    first_row[first_row_len] = '\0';
    char* method = (char*)malloc(10);
    char* path = (char*) malloc(100);
    char* protocol = (char*) malloc(100);

    sscanf(first_row, "%s %s %s", method, path, protocol);

    if(strlen(method) == 0 || strlen(path) == 0 || strlen(protocol) == 0){
        free(first_row);
        return 400;
    }
    if(((strcmp(protocol, "HTTP/1.0")) != 0 && strcmp(protocol, "HTTP/1.1") != 0)){
        free(first_row);
        return 400;
    }
    if(strstr(request, "Host: ") == NULL){
        free(first_row);
        return 400;
    }
    if(strcmp(method, "GET") != 0){
        free(first_row);
        return 501;
    }
    free(first_row);
    free(method);
    free(protocol);
    free(path);
    return 1;
}

char* to_binary(char* ip_string) {
    char *bin = (char *) malloc(IPV4_BINARY_LENGTH + 1);
    memset(bin, 0, IPV4_BINARY_LENGTH + 1);

    char ip_bytes_array[4][9];
    sscanf(ip_string, "%8[^.].%8[^.].%8[^.].%8s", ip_bytes_array[0], ip_bytes_array[1], ip_bytes_array[2],
           ip_bytes_array[3]);
    for (int i = 0; i < 4; ++i) {
        char *ret = token_to_binary((int) strtoul(ip_bytes_array[i], NULL, 10));
        strcat(bin, ret);
        free(ret);
    }
    return bin;
}

char* token_to_binary(int token){
    char * ret = (char*) malloc(9);
    for (int i = 0; i < 8; ++i) {
        ret[7-i] = (char)((token % 2) + '0');
        token /= 2;
    }
    ret[8] = '\0';
    return ret;
}

void write_to_socket(int sock_fd, char* msg, unsigned long bytes_to_write){
    if(sock_fd == -1){
        return;
    }
    unsigned long bytes_written = 0;
    unsigned long wrote;
    unsigned long left_bytes = bytes_to_write;
    while(bytes_written < bytes_to_write){
        wrote = write(sock_fd, msg + bytes_written, left_bytes);
        bytes_written += wrote;
        left_bytes -= wrote;
    }
}
void write_to_client_socket(int sock_fd, unsigned char* msg, unsigned long bytes_to_write) {

    if(sock_fd == -1){
        return;
    }
    unsigned long bytes_written = 0;
    unsigned long wrote;
    unsigned long left_bytes = bytes_to_write;
    while(bytes_written < bytes_to_write){
        wrote = write(sock_fd, msg + bytes_written, left_bytes);
        bytes_written += wrote;
        left_bytes -= wrote;
    }
}


void insert_host(char *line) {
    struct Node* newNode = (struct Node*)malloc(sizeof(struct Node));
    if (newNode == NULL) {
        perror("malloc\n");
        exit(EXIT_FAILURE);
    }
    if(line[0] >= '0' && line[0] <= '9'){ // if starts with number we know that this is an ip address
        char* mask_start = NULL;
        unsigned long mask_len = 32;
        if((mask_start = strstr(line, "/")) != NULL){
            mask_len = strtoul(mask_start + 1, NULL, 10);
            *mask_start = '\0';
        }
        char* ret = to_binary(line); // convert the specified token to bunary value
        memset(line, 0 , strlen(line));
        memcpy(line, ret, strlen(ret));
        mask_len = mask_len <= IPV4_BINARY_LENGTH ? mask_len : IPV4_BINARY_LENGTH;
        line[mask_len] = '\0';
        free(ret);
    }
    newNode->line = strdup(line); // Copy the line into the node
    newNode->next = NULL;
    if (filter_head == NULL) {
        filter_head = newNode;
    } else {
        struct Node* temp = filter_head;
        while (temp->next != NULL) {
            temp = temp->next;
        }
        temp->next = newNode;
    }
}

int search_host(char *line) {
    struct Node* current = filter_head;
    struct hostent *host_ip = gethostbyname(line);

    if (host_ip == NULL) {
        //printf("gethostbyname\n");
        return 1;
    }

    // Convert the IP address to a string format
    struct in_addr **addr_list = (struct in_addr **)host_ip->h_addr_list;
    char* ip_string =  to_binary(inet_ntoa(*addr_list[0]));
    while (current != NULL) {
        if (strcmp(current->line, line) == 0 || strncmp(ip_string, current -> line, strlen(current -> line)) == 0) {
            free(ip_string);
            return 1; // Found
        }
        current = current->next;
    }
    free(ip_string);
    return 0; // Not found
}

char* error_generator(int error_type){
    char * ret = (char*) malloc(1000);
    memset(ret, 0, 1000);
    char error_description[50];
    error_description[0] = '\0';
    char body_description[50];
    body_description[0] = '\0';

    struct timeval tv;
    gettimeofday(&tv, NULL);

    time_t current_time = tv.tv_sec;
    struct tm *time_info = gmtime(&current_time);
    char date[50];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", time_info);

    switch (error_type) {
        case 400:
            strcat(error_description, "400 Bad Request");
            strcat(body_description, "Bad Request.");
            break;
        case 403:
            strcat(error_description, "403 Forbidden");
            strcat(body_description, "Access denied.");
            break;
        case 404:
            strcat(error_description, "404 Not Found");
            strcat(body_description, "File not found.");
            break;
        case 500:
            strcat(error_description, "500 Internal Server Error");
            strcat(body_description, "Some server side error.");
            break;
        case 501:
            strcat(error_description, "501 Not supported");
            strcat(body_description, "Method is not supported.");
            break;
        default:
            return "";
    }
    char body[500];
    snprintf(body, sizeof(body), "<HTML><HEAD><TITLE>%s</TITLE></HEAD>\r\n"
                                 "<BODY><H4>%s</H4>\r\n"
                                 "%s\r\n"
                                 "</BODY></HTML>\r\n"
                                 , error_description, error_description, body_description);

    char headers[500];
    snprintf(headers, sizeof(body), "HTTP/1.1 %s\r\n"
                                    "Server: webserver/1.0\r\n"
                                    "Date: %s\r\n"
                                    "Content-Type: text/html\r\n"
                                    "Content-Length: %ld\r\n"
                                    "Connection: close\r\n\r\n"
            , error_description, date, strlen(body));

    strcat(ret, headers);
    strcat(ret, body);
    return ret;
}

void free_list(){
    struct Node * p = filter_head;
    struct Node* temp;

    while (p != NULL){
        free(p -> line);
        temp = p -> next;
        free(p);
        p = temp;
    }
}

void arguments_check(const int * port, const size_t * pool_size, const size_t* max_requests){
    if(*port <= 0 || *port > 65535){
        printf("Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        exit(EXIT_FAILURE);
    }
    if(*pool_size < 1){
        printf("Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        exit(EXIT_FAILURE);
    }
    if(*max_requests <= 0){
        printf("Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        exit(EXIT_FAILURE);
    }
}