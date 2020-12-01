#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "methods.h"

extern int log_fd;
extern int log_offset;
extern pthread_mutex_t log_mutex;

#define BUF_SIZE 8000

/*
 * This function checks using regex to see if the filename supplied
 * by the user is valid
 */
int valid_filename(char *filename)
{
    regex_t re;
    int ret;

    if(strlen(filename) != 27) {
        return 0;
    }

    const char *pattern = "^[a-zA-Z0-9_-]+$";
    if(regcomp(&re, pattern, REG_EXTENDED) != 0) {
        return 0;
    }

    ret = regexec(&re, filename, (size_t)0, NULL, 0);
    regfree(&re);

    if(ret == 0) {
        return 1;
    }

    return 0;
}

/*
 * base_response
 *
 * This function is called by other functions to write a given HTTP code to
 * a file descriptor. The functions below call this function.
 */
void base_response(int fd, int code, const char *status, const char *message, int _close)
{
    char reply[512];
    snprintf(reply,
      512,
      "HTTP/1.1 %d %s\r\n"
      "Content-Length: %d\r\n"
      "\r\n"
      "%s\r\n",
      code,
      status,
      (int)strlen(message) + 2,
      message);

    write(fd, reply, strlen(reply));
    if(_close)
        close(fd);
}

/*
 * HTTP 200
 */
void ok(int fd, const char *message, int close)
{
    base_response(fd, 200, "OK", message, close);
}

/*
 * HTTP 200 - write content length header for
 * GET request
 */
void ok_send_payload(int fd, int length)
{
    char reply[512];
    snprintf(reply,
      512,
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: %d\r\n"
      "\r\n",
      (int)length);

    write(fd, reply, strlen(reply));
}

/*
 * HTTP 201
 */
void created(int fd, const char *message)
{
    base_response(fd, 201, "Created", message, 1);
}

/*
 * HTTP 400
 */
void bad_request(int fd, const char *message)
{
    base_response(fd, 400, "Bad Request", message, 1);
}

/*
 * HTTP 403
 */
void forbidden(int fd, const char *message)
{
    base_response(fd, 403, "Forbidden", message, 1);
}

/*
 * HTTP 404
 */
void not_found(int fd, const char *message)
{
    base_response(fd, 404, "Not Found", message, 1);
}

/*
 * HTTP 500
 */
void internal_server_error(int fd, const char *message)
{
    base_response(fd, 500, "Internal Server Error", message, 1);
}

/*
 * This function replies to a GET request made by the client.
 */
void get(int fd, char *resource)
{
    char errbuf[140];

    // respond 400 if filename is not valid
    if(!valid_filename(resource)) {
        log_error("GET", resource, 400);
        bad_request(fd, "Invalid resource name");
        return;
    }

    if(access(resource, F_OK) != -1) {
        // respond 403 if server does not have permission to read file
        if(access(resource, R_OK) == -1) {
            log_error("GET", resource, 403);
            forbidden(fd, "No permission to read");
            return;
        }

        // read from file into buffer
        int filefd = open(resource, O_RDONLY);
        if(filefd < 0) {
            log_error("GET", resource, 500);
            char *err_msg = strerror_r(errno, errbuf, 140);
            internal_server_error(fd, err_msg);
            return;
        }

        log("GET", resource, 0);

        char buf[BUF_SIZE];
        int bytes_read, bytes_written, total_bytes_read;
        bytes_read = bytes_written = total_bytes_read = 0;
        struct stat st;
        fstat(filefd, &st); // get size of the file for content length
        int content_length = st.st_size;
        ok_send_payload(fd, content_length);

        do { // read and write into buffer
            bytes_read = read(filefd, buf, BUF_SIZE);
            if(bytes_read == -1) {
                // basically an unrecoverable error and we need to give up since we have
                // already written to log
                warn("Unrecoverable read error");
                close(fd);
                close(filefd);
                return;
            }
            total_bytes_read += bytes_read;
            bytes_written = write(fd, buf, bytes_read);
            if(bytes_written == -1) { // same here
                warn("Unrecoverable write error");
                close(fd);
                close(filefd);
                return;
            }
        } while(total_bytes_read < content_length);

        // use 200 OK function to send payload to fd
        close(fd);
        close(filefd);
    } else {
        not_found(fd, "Resource not available");
    }
}

/*
 * This function replies to a PUT request made by the client.
 */
void put(int fd, char *resource, int content_length)
{
    char errbuf[140];

    // the server will only wait for connections from the client for
    // five seconds
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

    // respond 400 if filename is not valid
    if(!valid_filename(resource)) {
        log_error("PUT", resource, 400);
        bad_request(fd, "Invalid resource name");
        return;
    }

    // respond 403 if server does not have permission to write to existing file
    if(access(resource, F_OK) != -1 && access(resource, W_OK) == -1) {
        log_error("PUT", resource, 403);
        forbidden(fd, "No permission to write");
        return;
    }

    // by default we will write an empty file
    char buf[BUF_SIZE];
    buf[0] = '\0';

    int bytes_read, total_bytes_read, bytes_written;
    bytes_read = bytes_written = total_bytes_read = 0;

    // write from payload to file
    int filefd = open(resource, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if(filefd < 0) {
        log_error("PUT", resource, 500);
        char *err_msg = strerror_r(errno, errbuf, 140);
        internal_server_error(fd, err_msg);
        return;
    }

    int offset = log("PUT", resource, content_length);

    if(content_length == 0) { // write empty file
        write(filefd, buf, strlen(buf));
    } else if(content_length < 0) { // content length was unspecified
        // read data from the fd and terminate in the buffer based on
        // either the total bytes read or the value of content-length
        do {
            bytes_read = read(fd, buf, BUF_SIZE);
            if(bytes_read == -1) { // unrecoverable
                warn("Unrecoverable read error");
                close(fd);
                close(filefd);
                return;
            }
            total_bytes_read += bytes_read;
            bytes_written = write(filefd, buf, bytes_read);
            if(bytes_written == -1) { // unrecoverable
                warn("Unrecoverable write error");
                close(fd);
                close(filefd);
                return;
            }
            // write to log
            offset = write_hex_to_log(bytes_read, total_bytes_read, offset, buf);
        } while(bytes_read == BUF_SIZE);
    } else {
        while(total_bytes_read < content_length) {
            // content length was specified so read up to the content length
            if((content_length - total_bytes_read) < BUF_SIZE)
                bytes_read = read(fd, buf, content_length - total_bytes_read);
            else
                bytes_read = read(fd, buf, BUF_SIZE);
            if(bytes_read == -1) { // unrecoverable
                warn("Unrecoverable read error");
                close(fd);
                close(filefd);
                return;
            }
            total_bytes_read += bytes_read;
            bytes_written = write(filefd, buf, bytes_read);
            if(bytes_written == -1) { // unrecoverable
                warn("Unrecoverable write error");
                close(fd);
                close(filefd);
                return;
            }
            offset = write_hex_to_log(bytes_read, total_bytes_read, offset, buf);
        }
    }

    if(offset != -1) {
        char separator[] = "========\n";
        pwrite(log_fd, separator, strlen(separator), offset);
    }

    close(filefd);
    created(fd, resource);
}

void log_get(char resource[28]);
int log_put(char resource[28], int content_length);

/*
 * This function is used to write a successful GET or PUT to the log
 */
int log(const char method[4], char resource[28], int content_length)
{
    if(!strcmp(method, "GET")) {
        log_get(resource);
        return 0;
    } else if(!strcmp(method, "PUT")) {
        return log_put(resource, content_length);
    }

    return 1;
}

/*
 * This function is called by int log() to write an entry for a GET request
 */
void log_get(char resource[28])
{
    if(log_fd < 0)
        return;

    // calculate how much space we have to reserve
    int size_of_first_line = 41;
    int size_of_separator = 9;
    int size_of_reservation = size_of_first_line + size_of_separator;

    int local_log_offset;
    pthread_mutex_lock(&log_mutex); // lock mutex to change offset to prevent race condition
    local_log_offset = log_offset;
    log_offset += size_of_reservation;
    pthread_mutex_unlock(&log_mutex);

    // write line at offset
    char *log_line = (char *)malloc(size_of_reservation + 1);
    snprintf(log_line, size_of_reservation + 1, "GET %s length 0\n========\n", resource);
    pwrite(log_fd, log_line, size_of_reservation, local_log_offset);
    free(log_line);
}

/*
 * This function is called by int log() to write the first line of the entry
 * for a PUT request. It returns the new offset so that the PUT method can
 * continuously write results from the buffer.
 */
int log_put(char resource[28], int content_length)
{
    if(log_fd < 0 || content_length < 0)
        return -1;

    int content_length_length = snprintf(NULL, 0, "%d", content_length);
    int size_of_first_line = 39 + content_length_length + 1;
    int size_of_content_line = 69;
    int amount_of_content_lines = content_length / 20;

    // remainder of content length times three - each hex char uses 2 digits
    // plus size of spaces in between each hex char + newline is also size
    // of remainder of content length
    int size_of_partial_content_line = 0;
    if(content_length % 20 > 0)
        size_of_partial_content_line = 9 + ((content_length % 20) * 3);
    int size_of_separator = 9;

    int size_of_reservation = size_of_first_line + (size_of_content_line * amount_of_content_lines)
                              + size_of_partial_content_line + size_of_separator;

    int local_log_offset;
    pthread_mutex_lock(&log_mutex);
    local_log_offset = log_offset;
    log_offset += size_of_reservation;
    pthread_mutex_unlock(&log_mutex);

    char *log_line = (char *)malloc(size_of_first_line + 1);
    if(log_line) {
        snprintf(log_line, size_of_first_line + 1, "PUT %s length %d\n", resource, content_length);
    }

    local_log_offset += pwrite(log_fd, log_line, size_of_first_line, local_log_offset);
    free(log_line);

    return local_log_offset;
}

/*
 * This function writes bytes fron content to the log as a formatted
 * hex line
 */
int write_hex_to_log(int bytes_read, int total_bytes_read, int offset, char *content)
{
    if(log_fd < 0)
        return -1;

    int i, j, bytes, char_cur, new_offset;
    bytes = total_bytes_read - bytes_read; // counter of what byte # we are at
    char_cur = 0;        // cursor pointing to the location in content that we are reading from
    new_offset = offset; // cursor pointing to the location in the log file we are writing to

    int lines = bytes_read / 20; // determine how many lines we have to write
    if(bytes_read % 20 > 0)
        lines++;

    char byte_number_fmt[10];
    for(i = 0; i < lines; i++) {                       // for each line we are writing to the log
        snprintf(byte_number_fmt, 10, "%08d ", bytes); // write current byte index to file
        new_offset += pwrite(log_fd, byte_number_fmt, strlen(byte_number_fmt), new_offset);
        int upto = 20;
        if(i == lines - 1) {
            upto = bytes_read - (20 * i); // upto is how many bytes are in the line we are writing
        }

        unsigned int byte;
        char hex_char[3];
        for(j = 0; j < upto; j++) {
            byte = content[char_cur++]; // cast byte to unsigned int for hex conversion
            snprintf(hex_char, 3, "%02x", byte);
            new_offset += pwrite(log_fd,
              hex_char,
              strlen(hex_char),
              new_offset); // write a single hex digit to the file

            if(j < upto - 1)
                pwrite(log_fd, " ", 1, new_offset++); // add spaces in between each hex digit
        }

        bytes += 20;                           // we are writing 20 bytes per line
        pwrite(log_fd, "\n", 1, new_offset++); // write newline to file in between each line
    }

    return new_offset; // return offset so PUT knows where to write the next set of bytes
}

/*
 * This function is used to write an error status to the log
 */
void log_error(const char method[4], char *resource, int code)
{
    if(log_fd < 0)
        return;

    int size_of_first_line = 10 + strlen(resource) + 27;
    int size_of_separator = 9;

    int size_of_reservation = size_of_first_line + size_of_separator;
    int local_log_offset;

    pthread_mutex_lock(&log_mutex); // allocate space for error line
    local_log_offset = log_offset;
    log_offset += size_of_reservation;
    pthread_mutex_unlock(&log_mutex);

    char *log_line = (char *)malloc(size_of_reservation + 1);
    if(log_line) {
        snprintf(log_line,
          size_of_reservation + 1,
          "FAIL: %s %s HTTP/1.1 --- response %d\n========\n",
          method,
          resource,
          code);
    }

    pwrite(log_fd, log_line, size_of_reservation, local_log_offset);
    free(log_line);
}
