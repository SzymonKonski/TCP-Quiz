#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <stdbool.h>

#define ERR(source) (perror(source),\
    fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
    exit(EXIT_FAILURE))


struct Queue {
    int front, rear, size;
    unsigned capacity;
    int* array;
};

int isFull(struct Queue* queue) {
    return (queue->size == queue->capacity);
}

struct Queue* createQueue(unsigned capacity)
{
    struct Queue* queue = (struct Queue*)malloc(
        sizeof(struct Queue));
    if(!queue) ERR("malloc");
    queue->capacity = capacity;
    queue->front = queue->size = 0;
 
    queue->rear = capacity - 1;
    queue->array = (int*)malloc(
        queue->capacity * sizeof(int));
    
    if(!queue->array) ERR("malloc");
    return queue;
}

int isEmpty(struct Queue* queue){
    return (queue->size == 0);
}
 
void enqueue(struct Queue* queue, int item){
    if (isFull(queue))
        return;
    queue->rear = (queue->rear + 1)
                  % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
}
 
int dequeue(struct Queue* queue){
    if (isEmpty(queue))
        return -1;
    int item = queue->array[queue->front];
    queue->front = (queue->front + 1)
                   % queue->capacity;
    queue->size = queue->size - 1;
    return item;
}
 
int sethandler( void (*f)(int), int sigNo) {
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1==sigaction(sigNo, &act, NULL))
    return -1;
    return 0;
}

int make_socket(char* name){
    int socketfd;
    if((socketfd = socket(PF_INET,SOCK_STREAM,0))<0) ERR("socket");
    return socketfd;
}

struct sockaddr_in make_address(char *address, char *port){
    int ret;
    struct sockaddr_in addr;
    struct addrinfo *result;
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    if((ret=getaddrinfo(address,port, &hints, &result))){
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }
    addr = *(struct sockaddr_in *)(result->ai_addr);
    freeaddrinfo(result);
    return addr;
}

int connect_socket(char *name, char *port){
    struct sockaddr_in  addr;
    int socketfd;
    socketfd = make_socket(name);
    addr=make_address(name,port);
    if(connect(socketfd,(struct sockaddr*) &addr,sizeof(struct sockaddr_in)) < 0){
        if(errno!=EINTR) ERR("connect");
        else {
            fd_set wfds ;
            int status;
            socklen_t size = sizeof(int);
            FD_ZERO(&wfds);
            FD_SET(socketfd, &wfds);
            if(TEMP_FAILURE_RETRY(select(socketfd+1,NULL,&wfds,NULL,NULL))<0) ERR("select");
            if(getsockopt(socketfd,SOL_SOCKET,SO_ERROR,&status,&size)<0) ERR("getsockopt");
            if(0!=status) ERR("connect");
        }
    }
    return socketfd;
}


void usage(char * name){
    fprintf(stderr,"USAGE: %s [address] [port] \n",name);
}

#define DATA_MAXSIZE 5
#define MAX_QUES_SIZE 2000

volatile sig_atomic_t do_work = 1;

struct connections {
    bool send;
    char msg;
    char* port;
    char* address;
    size_t question_len;
    char* question;
    int socket;
};

void sigint_handler(int sig) {
    do_work=0;
}

void close_connection(struct connections servers[], int i) {
    if(servers[i].socket != -1) {
        if(TEMP_FAILURE_RETRY(close(servers[i].socket))<0)ERR("close");
        servers[i].socket=-1;
        free(servers[i].address);
        free(servers[i].port);
        free(servers[i].question);
    }
}

char* read_from_stdin(size_t max_len) {
    char* read_buffer = malloc(sizeof(char) * DATA_MAXSIZE);
    memset(read_buffer, 0, max_len);
    ssize_t read_count = 0;
    ssize_t total_read = 0;
    int current_size = max_len;

    do {
        read_count = TEMP_FAILURE_RETRY(read(STDIN_FILENO, read_buffer+total_read, current_size-total_read));
        if (read_count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ERR("read");

        else if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) 
            break;
        
        else if (read_count > 0) {
            total_read += read_count;
            if (total_read >= current_size) {
                int size = current_size;
                char *tmp = realloc(read_buffer, sizeof(char) * (size*2));
                if (!tmp) ERR("realloc");
                read_buffer = tmp;
                memset(read_buffer+size, 0, size);
                current_size *= 2;
            }
        }
    } while (read_count > 0);

    return read_buffer;
}

int build_fd_sets(fd_set *read_fds, fd_set *write_fds, struct connections servers[], int servers_count) {
    FD_ZERO(read_fds);
    for (int i = 0; i < servers_count; ++i)
        if (servers[i].socket != -1)
            FD_SET(servers[i].socket, read_fds);

    FD_ZERO(write_fds);
    for (int i = 0; i < servers_count; ++i)
        if (servers[i].socket != -1 && servers[i].send)
            FD_SET(servers[i].socket, write_fds);

    FD_SET(STDIN_FILENO, read_fds);
    return 0;
}  

void handle_read_from_stdin(struct connections servers[], struct Queue* queue) {
    char* ans = read_from_stdin(DATA_MAXSIZE);
    if(!isEmpty(queue)) {
        int index = dequeue(queue);
        servers[index].send = true;
        servers[index].msg = ans[0];
    }
    else  printf("Not now\n");

    free(ans);
}

int receive_from_server(struct connections servers[], int i, int* active_servers, struct Queue* queue) {
    ssize_t max_read = MAX_QUES_SIZE - servers[i].question_len;
    ssize_t size = TEMP_FAILURE_RETRY(recv(servers[i].socket,servers[i].question+servers[i].question_len,max_read,0));
    
    if(size<0) ERR("read:");
    if(size == 0) {
        close_connection(servers, i);
        if(*active_servers == 1) {
            do_work = 0;
            return -1;
        }
        *active_servers-=1;
    }
    else {
        servers[i].question_len += size;
        ssize_t new_size = servers[i].question_len;
        
        if (new_size > 0 && servers[i].question[new_size-1] == '\0') {
            if(queue->size >= 1) {
                for(int i=0; i<queue->size; i++) {
                    int index = dequeue(queue);
                    servers[index].send = true;
                    servers[index].msg = '0';
                }
            }
            printf("Addres [%s] Port [%s]: %s\n", servers[i].address, servers[i].port, servers[i].question);            
            enqueue(queue, i);
        } 
        else if (strcmp(servers[i].question+servers[i].question_len-size, "END") == 0) 
            printf("END\n"); 

        else if(strcmp(servers[i].question+servers[i].question_len-size, "NO") == 0)  
            printf("NO\n"); 
    }
    return 0;
}

void send_to_server(struct connections servers[], int i) {
    if(servers[i].send) {
        if(TEMP_FAILURE_RETRY(send(servers[i].socket, &servers[i].msg, sizeof(char),0))<0&&errno!=EPIPE) ERR("write:");
        servers[i].send = false;
        servers[i].question_len = 0;
        memset(servers[i].question, 0, MAX_QUES_SIZE);
    }
}

void doClient(struct connections servers[], int servers_count, int max_fd){
    int active_servers = servers_count;
    fd_set read_fds;
    fd_set write_fds;
    struct Queue* queue = createQueue(servers_count);
    sigset_t mask, oldmask;
    if(sigemptyset(&mask)<0) ERR("sigemptyset");
    if(sigaddset(&mask, SIGINT)<0) ERR("sigaddset");
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    while(do_work){
        build_fd_sets(&read_fds, &write_fds, servers, servers_count);
        int activity = pselect(max_fd+1, &read_fds, &write_fds, NULL, NULL, &oldmask);
        if(activity > 0) {
            if (FD_ISSET(STDIN_FILENO, &read_fds)) 
                handle_read_from_stdin(servers, queue);
            
            for(int i=0; i<servers_count; i++) {
                if (FD_ISSET(servers[i].socket, &read_fds) && servers[i].socket != -1) {
                   if(receive_from_server(servers, i, &active_servers, queue) < 0)
                    break;  
                }

                if (FD_ISSET(servers[i].socket, &write_fds) && servers[i].socket != -1) 
                    send_to_server(servers, i);
            }
        }
        else {
            if(EINTR==errno)  continue;
            ERR("pselect");
        }
    }
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    free(queue->array);
    free(queue);
}

void prepare_connections(int servers_count, char** argv, int* max_fd, struct connections servers[]) {
    ssize_t address_len;
    ssize_t port_len;
    for(int i=0; i<servers_count; i++) {
        servers[i].socket = connect_socket(argv[1+2*i],argv[2*(i+1)]);
        if(servers[i].socket < 0) return;
        
        servers[i].send = false;

        if(servers[i].socket > *max_fd)
            *max_fd = servers[i].socket;
        servers[i].question = malloc(sizeof(char)*MAX_QUES_SIZE);
        if(!servers[i].question) ERR("malloc");
        memset(servers[i].question, 0, MAX_QUES_SIZE);

        address_len = strlen(argv[1+2*i]) + 1;
        port_len = strlen(argv[2*(i+1)]) +1;

        servers[i].address = malloc(address_len);
        if(!servers[i].address) ERR("malloc");
        strcpy(servers[i].address, argv[1+2*i]);

        servers[i].port = malloc(port_len);
        if(!servers[i].port) ERR("malloc");
        strcpy(servers[i].port, argv[2*(i+1)]);

        servers[i].question_len = 0;
        int flag = fcntl(servers[i].socket, F_GETFL, 0);
        flag |= O_NONBLOCK;
        if(fcntl(servers[i].socket, F_SETFL, flag) < 0) ERR("fcntl");
    }

    int flag = fcntl(STDIN_FILENO, F_GETFL, 0);
    flag |= O_NONBLOCK;
    if(fcntl(STDIN_FILENO, F_SETFL, flag)<0) ERR("fcntl");
}

int main(int argc, char** argv) {
    if(sethandler(SIG_IGN,SIGPIPE)) ERR("Seting SIGPIPE:");
    if(sethandler(sigint_handler,SIGINT)) ERR("Seting SIGINT:");
    int servers_count = (argc - optind)/2;
    if (servers_count == 0 || (argc - optind) % 2 != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int max_fd=STDIN_FILENO;
    struct connections servers[servers_count];
    prepare_connections(servers_count, argv, &max_fd, servers);
    doClient(servers, servers_count, max_fd);

    for(int i=0; i<servers_count; i++) 
        close_connection(servers, i);
    
    fprintf(stderr,"Client has terminated.\n");
    return EXIT_SUCCESS;
}