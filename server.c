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

#define BACKLOG 3

#define ERR(source) (perror(source),\
    fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
    exit(EXIT_FAILURE))

int sethandler(void (*f)(int), int sigNo) {
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        return -1;
    return 0;
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

int make_socket(int domain, int type){
    int sock;
    sock = socket(domain,type,0);
    if(sock < 0) ERR("socket");
    return sock;
}

int bind_tcp_socket(char* address, char* port){
    struct sockaddr_in addr;
    int socketfd,t=1;
    socketfd = make_socket(PF_INET,SOCK_STREAM);
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr = make_address(address, port);
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR,&t, sizeof(t))) ERR("setsockopt");
    if(bind(socketfd,(struct sockaddr*) &addr,sizeof(addr)) < 0)  ERR("bind");
    if(listen(socketfd, BACKLOG) < 0) ERR("listen");
    return socketfd;
}

int add_new_client(int sfd){
    int nfd;
    if((nfd=TEMP_FAILURE_RETRY(accept(sfd,NULL,NULL)))<0) {
        if(EAGAIN==errno||EWOULDBLOCK==errno) return -1;
        ERR("accept");
    }
    printf("New client\n");
    return nfd;
}

void usage(char * name){
    fprintf(stderr,"USAGE: %s [address] [port] [max_clients] [path to file]\n",name);
}

ssize_t bulk_read(int fd, char *buf, size_t count){
    int c;
    size_t len=0;
    do{
            c=TEMP_FAILURE_RETRY(read(fd,buf,count));
            if(c<0) return c;
            if(0==c) return len;
            buf+=c;
            len+=c;
            count-=c;
    }while(count>0);
    return len ;
}

struct connections{
    size_t read_bytes;
    size_t write_bytes;
    int questions_num;
    bool write;
    int socket;
};

#define DATA_MAXSIZE 5
#define MAXQ_LEN 2000
#define READ_BLOCK_SIZE 4096

volatile sig_atomic_t sig_alarm = 0;
volatile sig_atomic_t do_work = 1 ;
volatile sig_atomic_t sig_usr1 = 0;

void sigint_handler(int sig)  {
    do_work = 0;
}

void sigalrm_handler(int sig)  {
    sig_alarm = 1;
}

void sigusr1_handler(int sig) {
    sig_usr1 = 1;
}

int findFreeIndex(struct connections clients[], int max_clients) {
    for (int i = 0; i < max_clients; i++)
        if (clients[i].socket == -1)
            return i;

    return -1;
}

char** prep_question (int current_size) {
    char** questions = malloc(current_size * sizeof(char*));
    if(!questions) ERR("malloc");
    for (int i = 0; i < current_size; i++) {
        questions[i] = malloc(MAXQ_LEN * sizeof(char));
        if(!questions[i]) ERR("malloc");
        memset(questions[i], 0, MAXQ_LEN);
    }
    return questions;
}

void delete_endline(int numb, char** questions) {
    for(int i=0; i<numb; i++) {
        size_t len = strlen(questions[i] );
        if (len > 0 && questions[i][len - 1] == '\n') {
            questions[i][len - 1] = '\0';
        }
    }
}

char** read_questions(char* path, int *current_size, int *questions_numb) {
    char** questions = prep_question(*current_size);
    char buffer[READ_BLOCK_SIZE], c;
    ssize_t count;
    int current_file, numb = 0, t = -1;
    if ((current_file = TEMP_FAILURE_RETRY(open(path, O_RDONLY, 0777))) < 0) ERR("open");

    while (1) {
        if ((count = bulk_read(current_file, buffer, READ_BLOCK_SIZE)) > 0) {
            for (int i = 0; i < count; i++) {
                t++;
                c = buffer[i];
                if(c != '\n')  questions[numb][t] = c;
                else{
                    questions[numb][t] = '\n';
                    numb++;
                    if(numb == *current_size){
                        int size = *current_size;
                        char **tmp = realloc(questions, sizeof(char*) * (size*2));
                        if (!tmp) ERR("realloc");
                        questions = tmp;
                        for (int j = 0; j < size; j++) {
                            questions[size + j] = malloc(sizeof(char) * MAXQ_LEN);
                            if(!questions[size + j]) ERR("malloc");
                            memset (questions[size + j], 0, MAXQ_LEN);
                        }
                        *current_size *= 2;
                    }
                    t = -1;
                }
            }
            memset(buffer, 0, READ_BLOCK_SIZE);
        }
        else if (count == 0) break;
        else ERR("read failed");
    }
    delete_endline(numb, questions);
    *questions_numb = numb;
    return questions;
}

void handle_new_connection(int* max_fd, int listen, int* clients_num, int max_clients, 
int questions_numb, struct connections clients[], char** questions) {
    char no_msg[3] = "NO";
    int cfd = add_new_client(listen);

    if(cfd > 0) {
        if(cfd >= *max_fd)
            *max_fd = cfd;

        if(*clients_num >= max_clients){
            if(TEMP_FAILURE_RETRY(send(cfd,no_msg,sizeof(no_msg),0))<0&&errno!=EPIPE) ERR("write:");
            if(TEMP_FAILURE_RETRY(close(cfd))<0)ERR("close");
        }
        else{
            int new_flags;
            new_flags = fcntl(cfd, F_GETFL) | O_NONBLOCK;
            if(fcntl(cfd, F_SETFL, new_flags)<0) ERR("fcntl");
            *clients_num+=1;
            int i = findFreeIndex(clients, max_clients);
            clients[i].socket = cfd;
            clients[i].questions_num = rand()%questions_numb;
            clients[i].write_bytes = strlen(questions[clients[i].questions_num])+1;
            clients[i].write = true;
        }
    }    
}

void close_connection(struct connections clients[], int i) {
    if (TEMP_FAILURE_RETRY(close(clients[i].socket)) < 0) ERR("close");
    clients[i].socket = -1;
    clients[i].read_bytes = 0;
    clients[i].write_bytes = 0;
    clients[i].write = false;
}

void set_alarm() {
    struct itimerval ts;
    memset(&ts, 0, sizeof(struct itimerval));
    ts.it_value.tv_usec = 330000;
    if(setitimer(ITIMER_REAL,&ts,NULL)<0) ERR("settimer");
    sig_alarm = 0;
}

void send_to_client(struct connections clients[], int i, int *clients_num, char** questions) {
    int index = clients[i].questions_num;
    ssize_t size;

    if(clients[i].write_bytes>0) {
        int send_bytes = rand()%(clients[i].write_bytes)+1;
        if((size=TEMP_FAILURE_RETRY(send(clients[i].socket,(questions[index]+clients[i].read_bytes),send_bytes,0))) < 0) {
            if(errno == EPIPE){
                printf("Client finished\n");
                close_connection(clients, i);
                *clients_num-=1;
                return;
            } 
            else  ERR("write:");                        
        }
        clients[i].read_bytes += size;
        clients[i].write_bytes -= size;

        if(clients[i].write_bytes == 0)
            clients[i].write = false;
    }        
}

void receive_from_client(struct connections clients[], int i, int *clients_num, char** questions, int questions_numb) {
    char ans;
    ssize_t size = TEMP_FAILURE_RETRY(recv(clients[i].socket, &ans, sizeof(char), 0));
    if (size < 0) ERR("read:");
    if (size == 0) {
        printf("Client finished\n");
        close_connection(clients, i);
        *clients_num-=1;
    }
    else {
        printf("Read message %c\n", ans);
        clients[i].questions_num = rand()%questions_numb;
        clients[i].write_bytes = strlen(questions[clients[i].questions_num])+1;
        clients[i].read_bytes = 0;
        clients[i].write = true;
    }
}

void clear_clients(int max_clients, struct connections clients[]) {
    for(int i=0;i<max_clients;i++) {
        clients[i].socket = -1;
        clients[i].read_bytes = 0;
        clients[i].write_bytes = 0;
        clients[i].write = false;
    }
}

void close_server(int max_clients, struct connections clients[], int listen) {
    char end_msg[6] = "END";

    for(int i=0;i<max_clients;i++) {
        if(clients[i].socket != -1) {
            if(TEMP_FAILURE_RETRY(send(clients[i].socket,end_msg,sizeof(end_msg),0)) < 0 && errno!=EPIPE) ERR("write:");      
            close_connection(clients, i);
        }       
    }

    if(listen != -1) {
        if(TEMP_FAILURE_RETRY(close(listen))<0)ERR("close");
    }   
}

void set_mask(sigset_t* mask, sigset_t* oldmask) {
    if(sigemptyset (mask)<0) ERR("sigemptyset");
    if(sigaddset (mask, SIGINT) < 0)  ERR("sigaddset");
    if(sigaddset (mask, SIGALRM) < 0) ERR("sigaddset");
    if(sigaddset (mask, SIGUSR1) < 0) ERR("sigaddset");
    sigprocmask (SIG_BLOCK, mask, oldmask);
}

void free_question(int current_size, char** questions) {
    for (int i = 0; i < current_size; i++) 
        free(questions[i]);  
    free(questions);
}

void build_fd_sets(struct connections clients[], int max_clients, int* listen, fd_set *read_fds) {
    FD_ZERO(read_fds);
    for (int i = 0; i < max_clients; ++i)
        if (clients[i].socket != -1)
            FD_SET(clients[i].socket, read_fds);

    if(sig_usr1 == 0 && *listen != -1) FD_SET(*listen, read_fds);
    else if(sig_usr1 == 1 && *listen != -1)  {
        if(TEMP_FAILURE_RETRY(close(*listen))<0)ERR("close");
        sig_usr1 = 0;
        *listen = -1;
        printf("Stop does not accpet new clients\n");
    }      
}

void write_messages(struct connections clients[], int max_clients, char** questions, int* clients_num) {
    for(int i=0;i<max_clients;i++){
        if(clients[i].socket != -1 && clients[i].write == true) {
            send_to_client(clients, i, clients_num, questions);
        }
    }
}

void doServer(int listen, int max_clients, char* path){
    int questions_numb, current_size = DATA_MAXSIZE, clients_num = 0, max_fd = listen;
    struct connections clients[max_clients];
    char** questions = read_questions(path, &current_size, &questions_numb);
    clear_clients(max_clients, clients);
    fd_set read_fds;
    sigset_t mask, oldmask;
    set_mask(&mask, &oldmask);
    
    while(do_work){
        build_fd_sets(clients, max_clients, &listen, &read_fds);
        int active = pselect(max_fd+1,&read_fds,NULL,NULL,NULL,&oldmask);
        if(active > 0){
            if(FD_ISSET(listen,&read_fds))  {
                if(clients_num == 0) {
                    set_alarm();
                    printf("First client\n");
                }
                handle_new_connection(&max_fd, listen, &clients_num, max_clients, questions_numb, clients, questions);
            }

            for(int i=0;i<max_clients;i++)
                if(clients[i].socket != -1 && FD_ISSET(clients[i].socket,&read_fds)) 
                    receive_from_client(clients, i, &clients_num, questions, questions_numb);
        }
        else{
            if(errno == EINTR){
                if(do_work == 0 || sig_usr1==1) continue;
                if(sig_alarm == 1 && clients_num > 0) {
                    write_messages(clients, max_clients, questions, &clients_num);
                    set_alarm();
                }
            }
            else ERR("pselect");
        }
    }
    close_server(max_clients, clients, listen);
    free_question(current_size, questions);
    sigprocmask (SIG_UNBLOCK, &mask, NULL);
}

int main(int argc, char** argv) {

    int max_clients = atoi(argv[3]);
    
    if(argc!=5 || max_clients <= 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    srand(getpid());

    if(sethandler(SIG_IGN,SIGPIPE)) ERR("Seting SIGPIPE:");
    if(sethandler(sigint_handler,SIGINT)) ERR("Seting SIGINT:");
    if(sethandler(sigalrm_handler,SIGALRM)) ERR("Seting SIGALRM:");
    if(sethandler(sigusr1_handler,SIGUSR1)) ERR("Seting SIGUSR1:");

    int listen = bind_tcp_socket(argv[1], argv[2]);
    int new_flags = fcntl(listen, F_GETFL) | O_NONBLOCK;
    if(fcntl(listen, F_SETFL, new_flags)<0) ERR("fcntl");

    doServer(listen, max_clients, argv[4]);
   
    fprintf(stderr,"Server has terminated.\n");
    return EXIT_SUCCESS;
}