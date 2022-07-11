// general library
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>   /* struct timespec */
#include <unistd.h> /* close() */
#include <getopt.h>
#include <sys/types.h>
#include <signal.h>

// socket API
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NANOSEC 1000000000UL
#define TIMEOUT 2
#define MBYTE (1024*1024UL)

// Default configuration
size_t msg_len = 64;
char *server_ipaddr = "127.0.0.1";
unsigned int server_port = 7777;
unsigned int num_of_req = 100000;
int sock_fd;

// time difference
static double time_diff(struct timespec start, struct timespec end)
{
    return (double) ((end.tv_sec - start.tv_sec) * NANOSEC) + 
                    (end.tv_nsec - start.tv_nsec);
}

static void close_devices()
{
    close(sock_fd);
}

// handling error
void handle_error(const char *s)
{
    perror(s);
    exit(EXIT_FAILURE);
}

// print usage
void usage(char *proc)
{
    printf("Usage: %s -h <server ip> -p <port> -r <# of reqs> -d <data size> -t <testcase> \n", proc);
    printf("\t -d: data size to set (default: 64)\n");
    printf("\t -h: server IP (default: 127.0.0.1)\n");
    printf("\t -p: server Port (default: 7777)\n");
    printf("\t -r: the number of requests (default: 100,000)\n");
    printf("\t -t: test cases (set / get)\n");
    exit(EXIT_FAILURE);
}

static void gen_random_data(char *data, int count)
{
    static uint32_t state= 1234;
    int i = 0;

    while (count--) {
        state = (state*1103515245+12345);
        data[i++] = '0' + ((state>>16)&63);
    }
}

char *create_cmd(char *opt, char *data, int *key)
{
    char *cmd = malloc(4 + 6 + strlen(data));

    if (*key >= 16384)
        *key = 0;

    if (strcmp(opt, "set") == 0 || strcmp(opt, "SET") == 0) {
        sprintf(cmd, "SET:%d:%d", *key, *key);
    }
    else if (strcmp(opt, "get") == 0 || strcmp(opt, "GET") == 0) {
        sprintf(cmd, "GET:%d", *key);
    }
   
    (*key)++;

    return cmd;
}

void config_socket(int *fd, struct sockaddr_in *server_addr, 
    struct sockaddr_in *client_addr)
{
    struct timespec ttl = {TIMEOUT, 0};    // set timeout

    if ((*fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        handle_error("socket");
    // configure socket info
    memset(server_addr, 0, sizeof(*server_addr));
    server_addr->sin_family = AF_INET;
    server_addr->sin_addr.s_addr = inet_addr(server_ipaddr);
    server_addr->sin_port = htons(server_port);

    memset(client_addr, 0, sizeof(*client_addr));
    client_addr->sin_family = AF_INET;
    client_addr->sin_addr.s_addr = htons(INADDR_ANY);
    client_addr->sin_port = htons(8888);

    // bind
    if (-1 == bind(*fd, (struct sockaddr *) client_addr, sizeof (*client_addr)))
        handle_error("bind");

    // set receive UDP message timeout
    setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO,  (char*)&ttl, sizeof(struct timespec));
}

int main(int argc, char *argv[])
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t server_len = sizeof(server_addr);
    struct timespec start, end;
    int c;
    char *test= malloc(4);
    char *cmd = NULL;
    int datasize = 64;
    char *data = NULL;
    ssize_t recvlen;
    double total_time;
    int count = 0;

    while ((c = getopt(argc, argv, "h:p:r:d:t:m:")) != -1) {
        switch (c) {
            case 't':
                // set or get
                test = optarg;
                break;
            case 'h':
                server_ipaddr = optarg;
                break;
            case 'p':
                server_port = atoi(optarg);
                break;
            case 'r':
                num_of_req = atoi(optarg);
                break;
            case 'd':
                datasize = atoi(optarg);
                break;
            case 'm':
            default:
                usage(argv[0]);
        }
    }
    
    if (strlen(test) == 0)
        usage(argv[0]);

    // data for sending
    data = malloc(datasize);
    gen_random_data(data, datasize);

    config_socket(&sock_fd, &server_addr, &client_addr);

    // Benchmark starts
    for (int i = 0; i < num_of_req; i++) {
        ssize_t bytes_sent;
        int recv_msg_len = 1024;
        void *recv_msg = malloc(recv_msg_len);
        double timeuse = 0.0;
        clock_gettime(CLOCK_REALTIME, &start);

        cmd = create_cmd(test, data, &count);
        bytes_sent = sendto(sock_fd, cmd, strlen(cmd), 0, 
                (struct sockaddr *) &server_addr, server_len);

        memset(recv_msg, 0, recv_msg_len);

        /* Waiting message come back */
        recvlen = recvfrom(sock_fd, recv_msg, recv_msg_len, 0, 
                (struct sockaddr *)&server_addr, &server_len);

        if (recvlen >= 0) {
            clock_gettime(CLOCK_REALTIME, &end);

            printf("%d from %s:%d (server)\n", *(int *)recv_msg, inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));

            timeuse = time_diff(start, end) / 1000; // for microsecond
            total_time += timeuse;
        }
        else{
            printf("\nMessage Receive Timeout or Error\n");
            printf("\n---------------\n");
            continue;
        }
    }

    printf("Avg. RTT: %.3f us\n", total_time / num_of_req);
    printf("Throughput: %f MBytes/sec\n", (num_of_req * 4) * MBYTE / total_time / 1000000UL);

    close_devices();
    return 0;
}

