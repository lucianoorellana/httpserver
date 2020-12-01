#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "methods.h"
#include "queue.h"
#include "worker.h"

int log_offset;
int log_fd;

pthread_cond_t condl;
pthread_mutex_t mutex;
pthread_mutex_t log_mutex; // mutex for log offset

volatile sig_atomic_t listening = 1; // sentinel value to run server

/*
 * This function handles various termination signals. For SIGINT,
 * SIGQUIT or SIGTERM the software closes gracefully. For SIGHUP,
 * the software daemonizes by redirecting its output to log files.
 */
static void sig_handler(int signal)
{
    if(signal == SIGINT || signal == SIGQUIT || signal == SIGTERM) {
        listening = 0;
    }

    if(signal == SIGHUP) {
        // daemonize, start logging
        freopen("httpserver.access.log", "a", stdout);
        freopen("httpserver.error.log", "a", stderr);
    }
}

/*
 * The entry of the point of the application, which also contains the main
 * server loop.
 */
int main(int argc, char *argv[])
{
    int opt;
    int workers = 4; // default amount of worker threads is four

    log_offset = 0;
    log_fd = -1;

    while((opt = getopt(argc, argv, "W:l:")) != -1) {
        switch(opt) {
            case 'W': // flag for setting workers
                workers = atoi(optarg);
                break;
            case 'l': // flag for setting logfile
                if(!strcmp(optarg, "httpserver.access.log")
                   || !strcmp(optarg, "httpserver.error.log")) {
                    fprintf(stderr,
                      "%s: %s is a reserved filename, please name the log differently\n",
                      argv[0],
                      optarg);
                    exit(EXIT_FAILURE);
                }

                log_fd = open(optarg, O_RDWR | O_CREAT | O_TRUNC, 0644);
                if(log_fd < 0)
                    err(1, "%s", optarg);
                break;
            default: // '?'
                fprintf(stderr, "Usage: %s [-W workers] host [port]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if(workers < 1) {
        fprintf(stderr, "%s: at least one worker thread is needed to start the server\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if(optind >= argc) { // if optind >= argc then no host was specified
        fprintf(stderr, "Usage: %s [-W workers] host [port]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int i;
    pthread_t *thread;
    // allocate threads based on # of workers requested
    thread = (pthread_t *)malloc(workers * sizeof(pthread_t));

    pthread_cond_init(&condl, NULL);
    pthread_mutex_init(&mutex, NULL);

    struct queue *queue = new_queue();

    // all of these signals are masked from the worker
    // threads because they are handled by the main thread
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    if(pthread_sigmask(SIG_SETMASK, &set, NULL) < 0)
        warn("pthread_sigmask");

    // initialize all threads
    for(i = 0; i < workers; i++) {
        pthread_create(&thread[i], NULL, accept_job, queue);
    }

    // unmask signals from the main thread
    sigemptyset(&set);
    if(pthread_sigmask(SIG_SETMASK, &set, NULL) < 0)
        warn("pthread_sigmask");

    // these are all of the signals that are being
    // sent to the handler
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGTERM);
    struct sigaction a;
    a.sa_handler = sig_handler;
    a.sa_flags = 0;
    a.sa_mask = set;
    sigaction(SIGINT, &a, NULL);
    sigaction(SIGQUIT, &a, NULL);
    sigaction(SIGTERM, &a, NULL);

    signal(SIGHUP, sig_handler);
    signal(SIGUSR1, sig_handler);
    signal(SIGUSR2, sig_handler);

    int status;
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // ipv4
    hints.ai_socktype = SOCK_STREAM; // tcp

    char *port = (char *)malloc(6); // highest possible port number is 65535
    strncpy(port, "80", 6);         // default port is 80
    if(optind + 1 < argc)           // user specified a port
        port = argv[optind + 1];

    if((status = getaddrinfo(argv[optind], port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        exit(EXIT_FAILURE);
    } // resolve interface from user input

    // set up the socket
    int fd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if(fd == -1) {
        err(1, NULL);
    }

    // allow the server to recapture the socket (prevents already bound error)
    int yes = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0)
        err(1, "setsockopt(SO_REUSEADDR) error");

    if(bind(fd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        err(1, "failed to bind");
    }

    freeaddrinfo(servinfo);
    free(port);

    if(listen(fd, 10) == -1) {
        err(1, "failed to listen");
    }

    int new_conn;
    while(listening) {
        // accept new connection when it arrives
        new_conn = accept(fd, NULL, NULL);
        if(new_conn < 0) {
            if(errno != EINTR) // if errno == EINTR then the server is quitting
                warn("accept");
            continue;
        }
        enqueue(queue, new_conn); // add connection to the work queue
    }

    // if we broke out of the while loop, then a quit was requested
    printf("Quitting...\n");

    // send -2 to all worker threads, which is their signal to
    // terminate
    for(i = 0; i < workers; i++) {
        enqueue(queue, -2);
    }

    // wait for the threads to finish working
    for(i = 0; i < workers; i++) {
        pthread_join(thread[i], NULL);
    }

    close(fd);
    close(log_fd);

    free(thread);
    free(queue);

    return 0;
}
