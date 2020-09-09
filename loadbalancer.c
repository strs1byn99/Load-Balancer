#include<err.h>
#include<arpa/inet.h>
#include<netdb.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include<pthread.h>
#include <time.h>

#define QUEUE_SIZE 1000
#define BUFFER_SIZE 16384

int NUM_THREADS = 4;
int NUM_REQUEST = 5;
int CNT_REQUEST = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
// when no servers are up
char INTERNAL_SERVER_ERROR[100] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"; 

typedef struct server_info_t {
    int port;
    int alive; // initialize to false
    int total_request;  // update after healthcheck
    int error_request;  // update after healthcheck
} ServerInfo;

typedef struct servers_t {
    int num_servers;
    ServerInfo *servers;
} Servers;

Servers servers;

// Source: GeeksforGeeks
// C program for array implementation of queue 
struct Queue { 
    int front, rear, size; 
    unsigned capacity; 
    int* array; 
}; 
struct Queue* createQueue(unsigned capacity) { 
    struct Queue* queue = (struct Queue*) malloc(sizeof(struct Queue)); 
    queue->capacity = capacity; 
    queue->front = queue->size = 0;  
    queue->rear = capacity - 1;  // This is important, see the enqueue 
    queue->array = (int*) malloc(queue->capacity * sizeof(int)); 
    return queue; 
} 
int isFull(struct Queue* queue) {  return (queue->size == queue->capacity);  } 
int isEmpty(struct Queue* queue) {  return (queue->size == 0); } 
void enqueue(struct Queue* queue, int item) { 
    if (isFull(queue)) 
        return; 
    queue->rear = (queue->rear + 1)%queue->capacity; 
    queue->array[queue->rear] = item; 
    queue->size = queue->size + 1; 
} 
int dequeue(struct Queue* queue) { 
    if (isEmpty(queue)) 
        return 0; 
    int item = queue->array[queue->front]; 
    queue->front = (queue->front + 1)%queue->capacity; 
    queue->size = queue->size - 1; 
    return item; 
} 
int front(struct Queue* queue) { 
    if (isEmpty(queue)) 
        return 0; 
    return queue->array[queue->front]; 
} 
int rear(struct Queue* queue) { 
    if (isEmpty(queue)) 
        return 0; 
    return queue->array[queue->rear]; 
} 

struct Queue* queue;

int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd=socket(AF_INET,SOCK_STREAM,0);
    if (connfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(connectport);

    /* For this assignment the IP address can be fixed */
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0)
        return -1;
    return connfd;
}

int server_listen(int port) {
    int listenfd;
    int enable = 1;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        return -1;
    if (bind(listenfd, (struct sockaddr*) &servaddr, sizeof servaddr) < 0)
        return -1;
    if (listen(listenfd, 500) < 0)
        return -1;
    return listenfd;
}

int bridge_connections(int fromfd, int tofd) {
    char recvline[BUFFER_SIZE];
    int n = recv(fromfd, recvline, BUFFER_SIZE, 0);
    if (n < 0) {
        printf("connection error receiving\n");
        return -1;
    } else if (n == 0) {
        printf("receiving connection ended\n");
        return 0;
    }
    recvline[n] = '\0';
    n = send(tofd, recvline, n, 0);
    if (n < 0) {
        printf("connection error sending\n");
        return -1;
    } else if (n == 0) {
        printf("sending connection ended\n");
        return 0;
    }
    return n;
}

void bridge_loop(int sockfd1, int sockfd2) {
    fd_set set;
    struct timeval timeout;

    int fromfd, tofd;
    while(1) {
        // set for select usage must be initialized before each select call
        // set manages which file descriptors are being watched
        FD_ZERO (&set);
        FD_SET (sockfd1, &set);
        FD_SET (sockfd2, &set);

        // same for timeout
        // max time waiting, 5 seconds, 0 microseconds
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        // select return the number of file descriptors ready for reading in set
        switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                printf("error during select, exiting\n");
                send(sockfd1, INTERNAL_SERVER_ERROR, strlen(INTERNAL_SERVER_ERROR), 0);
                return;
            case 0:
                printf("both channels are idle, waiting again\n");
                send(sockfd1, INTERNAL_SERVER_ERROR, strlen(INTERNAL_SERVER_ERROR), 0);
                return;
                // continue;
            default:
                if (FD_ISSET(sockfd1, &set)) {
                    fromfd = sockfd1;
                    tofd = sockfd2;
                } else if (FD_ISSET(sockfd2, &set)) {
                    fromfd = sockfd2;
                    tofd = sockfd1;
                } else {
                    return;
                }
        }
        if ((bridge_connections(fromfd, tofd)) <= 0) {
            return; 
        }
    }
}

void do_periodic_healthcheck() {
    pthread_mutex_lock(&mutex);

    fd_set set;
    struct timeval timeout;

    char healthcheck[100] = "GET /healthcheck HTTP/1.1\r\nContent-Length: 0\r\n\r\n"; 
    char recvline[BUFFER_SIZE];
    memset(recvline, 0, sizeof(recvline));
    // perform healthcheck requests
    // update servers.servers
    for (int i = 0; i < servers.num_servers; ++i) {
        int serverfd = client_connect(servers.servers[i].port);
        int n = send(serverfd, healthcheck, strlen(healthcheck), 0);

        int idx = 0;
        while (n > 0) {
            FD_ZERO (&set);
            FD_SET (serverfd, &set);
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;

            switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
                case -1:
                    printf("error during select, exiting healthcheck\n");
                    servers.servers[i].alive = 0;
                    break;
                case 0:
                    printf("serverfd is idle, waiting again\n");
                    servers.servers[i].alive = 0;
                    break;
                default:
                    if (FD_ISSET(serverfd, &set)) {
                        n = recv(serverfd, &recvline[idx], 100, 0);
                        idx += n;
                    } else {
                        break; // this should be unreachable
                    }
            }
            
        }

        printf("%s\n", recvline);
        // parse recvline
        int e, t;
        int num_var = sscanf(recvline, "HTTP/1.1 200 OK\r\nContent-Length: %*d\r\n\r\n%d\n%d%*s", &e, &t);
        if (num_var == 2) {
            servers.servers[i].total_request = t;
            servers.servers[i].error_request = e;
            servers.servers[i].alive = 1;
        } else { // if healthcheck format is incorrect
            servers.servers[i].alive = 0;
        }
        memset(recvline, 0, sizeof(recvline));
    }
    
    CNT_REQUEST = 0;
    pthread_mutex_unlock(&mutex);
}

void init_servers(int argc, char **argv, int start_of_ports) {
    servers.num_servers = argc - start_of_ports;
    if (servers.num_servers <= 0) {
        printf("Error: please specify backend httpserver ports!\n");
        return;
    }
    printf("The number of servers is: %d\n", servers.num_servers);
    servers.servers = malloc(servers.num_servers*sizeof(ServerInfo));
    for (int port_idx = start_of_ports; port_idx < argc; ++port_idx) {
        int idx = port_idx - start_of_ports;
        if (atoi(argv[port_idx]) > 0)
            servers.servers[idx].port = atoi(argv[port_idx]);
        else 
            servers.servers[idx].port = 0;
        printf("Port Number %d: %d\n", idx, servers.servers[idx].port);
        servers.servers[idx].alive = 0; 
        servers.servers[idx].total_request = 0;
        servers.servers[idx].error_request = 0;
    }
    do_periodic_healthcheck();
}

void* healthcheck_thread(void *obj) {

    while (1) {
        

        int sec = 0, trigger = 4000;
        clock_t before = clock();

        while (sec < trigger && CNT_REQUEST < NUM_REQUEST) {
            /* busy waiting */
            clock_t diff = clock() - before;
            sec = diff * 1000 / CLOCKS_PER_SEC;

        } 
        // pthread_mutex_lock(&mutex);

        do_periodic_healthcheck();
        sec = 0;

        // pthread_mutex_unlock(&mutex);
    }
}

int determine_port() {
    int pport;
    for (int i = 0; i < servers.num_servers; i++) { // initialize
        if (servers.servers[i].alive){
            pport = i;
            break;
        }
        // if none is alive, return a 500 - TODO
        if (i == servers.num_servers-1)
            return -1; 
    }
    for (int i = pport; i < servers.num_servers; ++i) { // determin the best port
        if (servers.servers[i].total_request < servers.servers[pport].total_request) {
            if (servers.servers[i].alive)
                pport = i;
        } else if (servers.servers[i].total_request == servers.servers[pport].total_request) {
            if (servers.servers[i].error_request <= servers.servers[pport].error_request) {
                if (servers.servers[i].alive)
                    pport = i;
            } 
            // otherwise it's still the original priority
        } 
        // otherwise it's still the original priority
    }

    return pport;
}

void* worker_thread(void *obj) {

    while (1) {

        pthread_mutex_lock(&mutex);

        if (queue->size > 0) {

            int connfd, acceptfd;
            acceptfd = dequeue(queue);
            ++CNT_REQUEST;

            // determine priority port
            int pport = determine_port();
            if (pport == -1) { // no servers up
                send(acceptfd, INTERNAL_SERVER_ERROR, strlen(INTERNAL_SERVER_ERROR), 0);
                pthread_mutex_unlock(&mutex);
                continue;
            }
            printf("pport choice is: %d\n", pport);
            
            // if ((connfd = client_connect(servers.servers[pport].port)) < 0) {
            //     err(1, "failed connecting");
            // }
            while ((connfd = client_connect(servers.servers[pport].port)) < 0) {
                servers.servers[pport].alive = 0;
                pport = determine_port();
                if (pport == -1) break;
            }
            if (pport == -1) { // no servers up
                send(acceptfd, INTERNAL_SERVER_ERROR, strlen(INTERNAL_SERVER_ERROR), 0);
                pthread_mutex_unlock(&mutex);
                continue;
            }
            bridge_loop(acceptfd, connfd);
        }


        pthread_mutex_unlock(&mutex);
    }
}

int main(int argc,char **argv) {
    int connfd, listenfd, acceptfd;
    uint16_t connectport, listenport;
    queue = createQueue(QUEUE_SIZE);

    if (argc < 3) {
        printf("missing arguments: usage %s port_to_connect port_to_listen", argv[0]);
        return 1;
    }

    /* get opt */
    int opt;
    if (argc > 1) {
        while ((opt = getopt(argc, argv, "N:R:")) > 0) {
            if (opt == 'N') {
                NUM_THREADS = atoi(optarg);
            } else if (opt == 'R') {
                NUM_REQUEST = atoi(optarg);
            } else {
                printf("Invalid opt: %c\n", opt);
            }
        }
        if (argv[optind] != NULL) {
            listenport = atoi(argv[optind]);
            printf("load balancer port: %d\n" , listenport);
            ++optind;
        }
    }

    if ((listenfd = server_listen(listenport)) < 0) // load balencer / listener
        err(1, "failed listening");

    init_servers(argc, argv, optind);

    pthread_t thread_hc;
    pthread_t thread_ids[NUM_THREADS];
    pthread_create(&thread_hc, NULL, &healthcheck_thread, NULL);
    for (int idx = 0; idx < NUM_THREADS; ++idx) {
        pthread_create(&thread_ids[idx], NULL, &worker_thread, NULL);
    }

    /* number of concurrent requests => number of loop accepting listenfd */
    while (1) {
        acceptfd = accept(listenfd, NULL, NULL);
        if (acceptfd < 0) { err(1, "failed accepting"); }
        if (acceptfd > 0) {
            if (queue->size < QUEUE_SIZE) {
                pthread_mutex_lock(&mutex);
                enqueue(queue, acceptfd);
                pthread_mutex_unlock(&mutex);
            } else {
                printf("too much requests in queue\n");
            }
        }
    }

    pthread_mutex_destroy(&mutex);
    free(queue);
    return 0;
}