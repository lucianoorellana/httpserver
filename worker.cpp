#include <err.h>
#include <iostream>
#include <string.h>
#include <unistd.h>

#include "methods.h"
#include "queue.h"
#include "worker.h"

#define BUF_SIZE 8000

void *accept_job(void *queue)
{
    int fd;
    int bytes_read;
    struct queue *request_queue = (struct queue *)queue;
    char buf[BUF_SIZE];

    for(;;) {
        fd = dequeue(request_queue);
        if(fd == -2) { // recieved kill signal
            break;
        }

        // read initial http request from the client
        bytes_read = read(fd, buf, BUF_SIZE);
        if(bytes_read == -1) {
            warn("Could not read from socket");
            continue;
        }

        // terminate the data at the amount of bytes recieved from the client
        buf[bytes_read] = '\0';

        char *token1 = NULL;
        char *saveptr1;
        // we are using strtok here to split each line of the http
        // request into a separate header to be processed individually
        // in the while loop
        token1 = strtok_r(buf, "\r\n", &saveptr1);

        int content_length = -1; // -1 is a sentinel value for no content
                                 // length supplied
        char *resource = NULL;   // this variable will store the filename

        while(token1 != NULL) {
            char *token2 = NULL;
            char *saveptr2;
            // we are using strtok here to split each header of the http
            // request by space, for example to split "Content-Length:" and "2"
            token2 = strtok_r(token1, " ", &saveptr2);
            while(token2 != NULL) {
                if(!strcmp(token2, "GET") || !strcmp(token2, "PUT")) {
                    resource = strtok_r(NULL, " ", &saveptr2);
                }

                if(!strcmp(token2, "Content-Length:")) {
                    content_length = atoi(strtok_r(NULL, " ", &saveptr2));
                }

                token2 = strtok_r(NULL, " ", &saveptr2);
            }

            token1 = strtok_r(NULL, "\r\n", &saveptr1);
        }

        if(!strncmp("GET", buf, 3)) {
            // if the user has given us a GET request, process in get()
            printf("GET %s\n", resource);
            get(fd, resource);
        } else if(!strncmp("PUT", buf, 3)) {
            // if the user has given us a PUT request, process in put()
            printf("PUT %s\n", resource);
            // send data to the put() function to be written to the disk
            put(fd, resource, content_length);
        } else {
            // if not PUT or GET, reply with 400 Bad Request
            bad_request(fd, "Unsupported method");
        }
    }

    return 0;
}
