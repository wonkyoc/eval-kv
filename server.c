// general library
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>   /* struct timespec */
#include <unistd.h> /* close() */
#include <getopt.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

// socket API
//#include <linux/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/net_tstamp.h>
#include <sys/ioctl.h>
#include <net/if.h>

#define NANOSEC 1000000000UL
#define MBYTE (1024*1024UL)
#define TIMEOUT 2
#define FPGA_PATH "/sys/bus/pci/devices/0000:b3:00.0/resource0_wc"
#define MMAP_SIZE 64*1024UL
#define SET 0
#define GET 1

#define CPU 0
#define FPGA 1

//#define DEBUG

struct command {
    int opt;
    void *key;
    void *value;
    int value_len;
}; 
extern int errno;
// Default configuration
size_t msg_len = 64;
char *server_ipaddr = "127.0.0.1";
unsigned int server_port = 7777;
unsigned int num_of_req = 100000;
int timeout = 10;
int verbose = 0;

// Vars for bandwidth
size_t total_pkt;
long long total_bytes_recv = 0;
struct timespec bw_start;


int sock_fd = -1;
int fpga_fd = -1;
char *f_buf = NULL;

struct scm_timestamping {
    struct timespec ts[3];
};

// time difference
static double time_diff(struct timespec start, struct timespec end)
{
    return (double) ((end.tv_sec - start.tv_sec) * NANOSEC) + 
                    (end.tv_nsec - start.tv_nsec);
}

static void close_devices()
{
    if (fpga_fd >= 0) {
        munmap((void *)f_buf, MMAP_SIZE);
        close(fpga_fd);
    }

    close(sock_fd);
}

void handle_error(const char *s)
{
    perror(s);
    printf("%d\n", errno);
    exit(EXIT_FAILURE);
}

void print_result()
{
    struct timespec end;

    clock_gettime(CLOCK_REALTIME, &end);
    double elapsed_time = time_diff(bw_start, end) / NANOSEC;
    printf("Throughput: %f MBytes/sec\n", 
            1.0 * total_bytes_recv / MBYTE / elapsed_time);
    printf("Total pkt %zu\n", total_pkt);
}

static struct timespec handle_time(struct msghdr *msg) {
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
    struct scm_timestamping *ts = (struct scm_timestamping *)CMSG_DATA(cmsg);
    return ts->ts[0];
}

void config_socket(int *fd, struct sockaddr_in *server_addr, 
    struct sockaddr_in *client_addr)
{
    int val = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE
        | SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RAW_HARDWARE;

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

    //setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMPING, &val, sizeof(val));

    if (bind(sock_fd, (struct sockaddr *) server_addr, sizeof(struct sockaddr_in)) < 0)
        handle_error("bind");
}

struct command *parse(char *data)
{
    struct command *cmd = malloc(sizeof(struct command));
    char *token = NULL;
    int temp = 0;

    cmd->key = NULL;
    cmd->opt = -1;
    cmd->value = NULL;
    cmd->value_len = 0;

    token = strtok(data, ":");
    if (strcmp(token, "SET") == 0) {
        cmd->opt = SET;
        token = strtok(NULL, ":");
        cmd->key = malloc(4);
        temp = atoi(token);
        memcpy(cmd->key, &temp, 4);
        token = strtok(NULL, ":");
        cmd->value = malloc(1024);
        cmd->value_len = 1024;
        temp = atoi(token);
        memcpy(cmd->value, &temp, 4);
        //cmd->value_len = strlen(cmd->value.c);
    }
    else if (strcmp(token, "GET") == 0) {
        cmd->opt = GET;
        token = strtok(NULL, ":");
        cmd->key = malloc(4);
        temp = atoi(token);
        memcpy(cmd->key, &temp, 4);
    }

#ifdef DEBUG
    printf("opt: %d key: %d value: %d value_len: %d\n",
            cmd->opt, *(int*)cmd->key, *(int*)cmd->value, cmd->value_len);
#endif

    return cmd;
}

void process_in_cpu(struct command *cmd, char **addr) {
        if (cmd->opt == SET) {
            memcpy(addr[*(int*)(cmd->key)], (int*) cmd->value, 4);
        }
        else if (cmd->opt == GET) {
            cmd->value = malloc(1024);
            memcpy(cmd->value, addr[*(int*)(cmd->key)], 4);
        }
}
void process_in_fpga(struct command *cmd, char *addr)
{
    int enable = 1;
    if (cmd->opt == SET) {
        // SET
        memcpy(addr, cmd->key, 4);      // Key
        memcpy(addr+4, cmd->value, 4); // Value
        memcpy(addr+8, (char *) &enable, 4);  // Write enable
    }
    else if (cmd->opt == GET) {
        // GET
        //printf("%s\n", cmd->i_key);
        memcpy(addr+12, cmd->key, 4);  // Key
        memcpy(addr+20, (char *) &enable, 4);  // Read enable

        // Polling
        while (1) {
            // slv_reg6 == 1 if the read data is ready from FPGA
            if (*(addr+24) == 1) {
                cmd->value = malloc(256);
                memcpy(cmd->value, addr+16, 4);

                // disable polling bit
                memset(addr+24, 0, 4);
                break;
            }
            printf("Polling...%d\n", *(addr+24));
        }
    }
}

int main(int argc, char *argv[])
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    unsigned int server_port;
    socklen_t client_addrlen;
    struct timespec start;
    struct timespec t1, t2, t3, t4;
    double t_recv = 0 , t_parse = 0, t_proc = 0, t_send = 0;
    double total_t_recv = 0 , total_t_parse = 0, 
           total_t_proc = 0, total_t_send = 0;
    struct command *cmd;
    char *fpga_addr, **cpu_addr;

    int recv_msg_len = 1024;
    char *recv_msg = malloc(recv_msg_len);

    int num_received;
    int c;
    long count = 0;

    while ((c = getopt(argc, argv, "xh:t:p:r:s:")) != -1) {
        switch (c) {
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
                msg_len = atoi(optarg);
                break;
            case 'x':
                fpga_fd = open(FPGA_PATH, O_RDWR);
                if (fpga_fd < 0)
                    handle_error("open");
                break;
            default:
                printf("...");
                exit(EXIT_FAILURE);
                // usage(argv[0]);
        }
    }

    // fpga buffer allocation
    if (fpga_fd >= 0) {
        fpga_addr = mmap(NULL, MMAP_SIZE, 
                PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fpga_fd, 0);
    }

    // CPU-side array
    cpu_addr = malloc(MMAP_SIZE * sizeof(char *));
    for (int index = 0; index < MMAP_SIZE; index++)
        cpu_addr[index] = malloc(4);

    // Configure socket
    config_socket(&sock_fd, &server_addr, &client_addr);

    while (1) {
        // part: receive data from client
        memset(recv_msg, 0, recv_msg_len);
        clock_gettime(CLOCK_REALTIME, &start);

        num_received = recvfrom(sock_fd, recv_msg, recv_msg_len, 0,
                (struct sockaddr *) &client_addr, &client_addrlen);
        clock_gettime(CLOCK_REALTIME, &t1);
        t_recv = time_diff(start, t1) / 1000;

        // part: parsing command
        cmd = parse(recv_msg);
        clock_gettime(CLOCK_REALTIME, &t2);
        t_parse = (time_diff(start, t2) - t_recv) / 1000;

        // part: processing key-value
        if (fpga_fd >= 0)
            process_in_fpga(cmd, fpga_addr);
        else
            process_in_cpu(cmd, cpu_addr);

        clock_gettime(CLOCK_REALTIME, &t3);
        t_proc = (time_diff(start, t3) - t_parse) / 1000;

        // part: send data to client
        // 256 is just a random small size to contain return value
        if (sendto(sock_fd, cmd->value, 256, 0,
                    (struct sockaddr *) &client_addr, client_addrlen) < 0) {
            perror("\nMessage Send Failed\n");
            fprintf(stderr, "Value of errno: %d\n", errno);
        }
        clock_gettime(CLOCK_REALTIME, &t4);
        t_send = (time_diff(start, t4) - t_proc) / 1000;

        // discard a few samples due to outliers
        if (count > 10) {
            total_t_recv += t_recv;
            total_t_parse += t_parse;
            total_t_proc += t_proc;
            total_t_send += t_send;
        }

        if (count == 99999) {
            printf("%.2f %.2f %.2f %.2f\n",
                    total_t_recv / count, 
                    total_t_parse / count,
                    total_t_proc / count,
                    total_t_send/ count);
            total_t_recv = total_t_parse = total_t_proc = total_t_send = 0;
            count = 0;
        }
        count++;
    }

   
    // free array
    for (int index = 0; index < MMAP_SIZE; index++)
        free(cpu_addr[index]);
    free(cpu_addr);

    close_devices();

    return 0;
}

