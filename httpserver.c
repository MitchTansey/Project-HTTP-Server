// Asgn 2: A simple HTTP server.
// By: Eugene Chou
//     Andrew Quinn
//     Brian Zhao

#include "asgn2_helper_funcs.h"
#include "connection.h"
#include "response.h"
#include "request.h"
#include "queue.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/file.h>

pthread_mutex_t glock;
//pthread_mutex_t mutex;
//pthread_cond_t cv;

void handle_connection(int);

void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);
void *cre_work(void *queue);

int main(int argc, char **argv) {
    if (argc < 2) {
        warnx("wrong arguments: %s port_num", argv[0]);
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    pthread_mutex_init(&glock, NULL);
    //pthread_mutex_init(&mutex, NULL);
    //pthread_cond_init(&cv, NULL);

    size_t port;
    char *endptr = NULL;
    int N = 4;
    int opt = getopt(argc, argv, "t:");
    switch (opt) {
    case 't':
        N = atoi(optarg);
        port = (size_t) strtoull(argv[3], &endptr, 10);

        if (endptr && *endptr != '\0') {
            warnx("invalid port number: %s", argv[3]);
            return EXIT_FAILURE;
        }

        break;

    default:
        port = (size_t) strtoull(argv[1], &endptr, 10);

        if (endptr && *endptr != '\0') {
            warnx("invalid port number: %s", argv[1]);
            return EXIT_FAILURE;
        }

        break;
    }

    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[2]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    queue_t *q = queue_new(N);
    pthread_t workers[N];

    for (int i = 0; i < N; i++) {
        pthread_create(&workers[i], NULL, &cre_work, (void *) q);
    }

    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        queue_push(q, (void *) connfd);
        //pthread_cond_signal(&cv);
    }

    return EXIT_SUCCESS;
}

void *cre_work(void *queue) {
    queue_t *q = (queue_t *) queue;
    uintptr_t conn;

    while (true) {
        queue_pop(q, (void **) &conn);

        handle_connection((int) conn);
        close((int) conn);
    }

    return 0;
}

void handle_connection(int connfd) {

    conn_t *conn = conn_new(connfd);

    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_GET) {
            handle_get(conn);
        } else if (req == &REQUEST_PUT) {
            handle_put(conn);
        } else {
            handle_unsupported(conn);
        }
    }
    conn_delete(&conn);
}

void handle_get(conn_t *conn) {

    pthread_mutex_lock(&glock);

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;

    int fd = open(uri, O_RDWR, 0600);

    char *r_id = "0";
    char *r_id_str = conn_get_header(conn, "Request-Id");
    if (r_id_str != NULL) {
        r_id = r_id_str;
    }

    if (errno == EISDIR || errno == EACCES) {
        res = &RESPONSE_FORBIDDEN;
        conn_send_response(conn, res);
        fprintf(stderr, "GET,%s,403,%s\n", uri, r_id);
        return;
    }

    if (fd < 0) {
        res = &RESPONSE_NOT_FOUND;
        conn_send_response(conn, res);
        fprintf(stderr, "GET,%s,404,%s\n", uri, r_id);
        return;
    }

    flock(fd, LOCK_SH);

    struct stat st;
    stat(uri, &st);

    pthread_mutex_unlock(&glock);

    res = &RESPONSE_OK;
    conn_send_file(conn, fd, st.st_size);
    fprintf(stderr, "GET,%s,200,%s\n", uri, r_id);
    close(fd);
    return;
}

void handle_unsupported(conn_t *conn) {
    char *uri = conn_get_uri(conn);

    char *r_id = "0";
    char *r_id_str = conn_get_header(conn, "Request-Id");
    if (r_id_str != NULL) {
        r_id = r_id_str;
    }

    // send responses
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);

    fprintf(stderr, "%s,%s,501,%s\n", request_get_str(&REQUEST_UNSUPPORTED), uri, r_id);
    return;
}

void handle_put(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;

    pthread_mutex_lock(&glock);
    // Check if file already exists before opening it.
    bool existed = access(uri, F_OK) == 0;

    char *r_id = "0";
    char *r_id_str = conn_get_header(conn, "Request-Id");
    if (r_id_str != NULL) {
        r_id = r_id_str;
    }

    // Open the file..
    int fd = open(uri, O_CREAT | O_WRONLY, 0600);

    if (fd < 0) {
        if (errno == EACCES || errno == EISDIR || errno == ENOENT) {
            res = &RESPONSE_FORBIDDEN;
            conn_send_response(conn, res);
            fprintf(stderr, "PUT,%s,403,%s\n", uri, r_id);
            pthread_mutex_unlock(&glock);
            return;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            conn_send_response(conn, res);
            fprintf(stderr, "PUT,%s,500,%s\n", uri, r_id);
            pthread_mutex_unlock(&glock);
            return;
        }
    }

    flock(fd, LOCK_EX);

    pthread_mutex_unlock(&glock);

    ftruncate(fd, 0);

    res = conn_recv_file(conn, fd);

    if (res == NULL && existed) {
        res = &RESPONSE_OK;
        conn_send_response(conn, res);
        fprintf(stderr, "PUT,%s,200,%s\n", uri, r_id);
        close(fd);
        return;
    } else if (res == NULL && !existed) {
        res = &RESPONSE_CREATED;
        conn_send_response(conn, res);
        fprintf(stderr, "PUT,%s,201,%s\n", uri, r_id);
        close(fd);
        return;
    }
}
