#include "seamless.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_EVENTS 100

// Externs.
extern int admin_fd;
extern int die_worker;
extern int server_fd;

// Forward decls.
int send_sockets();

void handle_sigusr1(int signum) {
    printf("worker: handling sigusr1\n");
}

int run_worker() {
    printf("worker: registering sigusr1 signal handler\n");
    struct sigaction usr1;
    memset(&usr1, 0, sizeof(struct sigaction));
    usr1.sa_handler = handle_sigusr1;
    int res_sig = sigaction(SIGUSR1, &usr1, NULL);
    if (res_sig == -1) {
        perror("worker: failed to register sigusr1 handler");
        return -1;
    }

    printf("worker: creating epoll instance\n");
    int poll_fd = epoll_create1(0);
    if (poll_fd == -1) {
        perror("worker: failed to create epoll instance");
        return -1;
    }

    printf("worker: registering admin fd with epoll\n");
    struct epoll_event admin_event;
    memset(&admin_event, 0, sizeof(admin_event));
    admin_event.events = EPOLLIN|EPOLLRDHUP|EPOLLET;
    admin_event.data.fd = admin_fd;
    int res_ctl = epoll_ctl(poll_fd, EPOLL_CTL_ADD, admin_fd, &admin_event);
    if (res_ctl == -1) {
        perror("worker: failed to add admin fd to epoll");
        return -1;
    }

    printf("worker: registering server fd with epoll\n");
    struct epoll_event server_event;
    memset(&server_event, 0, sizeof(server_event));
    server_event.events = EPOLLIN|EPOLLRDHUP|EPOLLET;
    server_event.data.fd = server_fd;
    res_ctl = epoll_ctl(poll_fd, EPOLL_CTL_ADD, server_fd, &server_event);
    if (res_ctl == -1) {
        perror("worker: failed to add server fd to epoll");
        return -1;
    }

    // Poll for events until worker is told to die.
    int conn_fd = 0;

    while (!die_worker) {
        struct epoll_event events[MAX_EVENTS];
        int num_events = epoll_wait(poll_fd, events, MAX_EVENTS, POLL_TIMEOUT_MS);
        if (num_events == 0) {
            printf("worker: no events during waiting period\n");
            continue;
        }

        while (num_events-- > 0) {
            struct epoll_event event = events[num_events];
            if (event.data.fd == admin_fd) {
                printf("worker: handling admin event\n");

                // Accept connection on admin socket.
                struct sockaddr_in conn;
                memset(&conn, 0, sizeof(conn));
                socklen_t conn_len = sizeof(conn);
                printf("worker: accepting admin connection\n");
                conn_fd = accept(admin_fd, (struct sockaddr *)&conn, &conn_len);
                if (conn_fd == -1) {
                    perror("worker: failed to accept admin connection");
                    return -1;
                }

                // Set accepted connection to non-blocking.
                int flags = fcntl(conn_fd, F_GETFL, 0);
                if (fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                    perror("worker: failed to set non-blocking flag on worker connection");
                    return -1;
                }

                // Register connection with epoll instance.
                printf("worker: adding connection to epoll\n");
                struct epoll_event eadd;
                memset(&eadd, 0, sizeof(eadd));
                eadd.events = EPOLLIN|EPOLLRDHUP|EPOLLET;
                eadd.data.fd = conn_fd;
                if (epoll_ctl(poll_fd, EPOLL_CTL_ADD, conn_fd, &eadd) == -1) {
                    perror("worker: failed to add admin fd to epoll");
                    return -1;
                }
            } else if (event.data.fd == server_fd) {
                printf("worker: processing server event\n");

                // Accept connection and close it immediately. We do no real
                // work on the server.
                struct sockaddr_in conn;
                socklen_t conn_len = sizeof(conn);
                printf("worker: accepting server connection\n");
                int conn_fd = accept(server_fd, (struct sockaddr *)&conn, &conn_len);
                if (conn_fd == -1) {
                    perror("worker: failed to accept server connection");
                    return -1;
                }

                // Close connection.
                printf("worker: closing server connection\n");
                close(conn_fd);
            } else if (event.data.fd == conn_fd) {
                for (;;) {
                    char buf[64];
                    memset(&buf, 0, sizeof(buf));
                    int nbytes = read(conn_fd, buf, sizeof(buf));
                    if (nbytes >= 0) {
                        if (strcmp(buf, MSG_GET_SOCKS) == 0) {
                            printf("worker: received get socks msg\n");
                            if (send_sockets() == -1) {
                                perror("worker: failed to send sockets");
                                exit(EXIT_FAILURE);
                            }
                        } else {
                            printf("worker: ignoring unsupported msg\n");
                        }
                    // Not a real error: nothing left to read.
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        printf("worker: read all data from client connection\n");
                        break;
                    } else {
                        perror("worker: failed to read data from client connection");
                        break;
                    }
                }

                // Close client connection.
                printf("worker: deleting conn_fd from epoll\n");
                if (epoll_ctl(poll_fd, EPOLL_CTL_DEL, conn_fd, NULL) == -1) {
                    perror("worker: failed to delete conn_fd from epoll");
                    return -1;
                }

                printf("worker: closing client connection\n");
                if (close(conn_fd) == -1) {
                    perror("worker: failed to close client connection");
                    return -1;
                }
            } else {
                printf("worker: processed unknown event\n");
            }
        }
    }
}

int send_sockets() {
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = SOCK_PATH
    };

    if (connect(admin_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(admin_fd);
        return -1;
    }

    printf("connected to admin sock\n");
    struct iovec iov = {
        .iov_base = ":)", // Must send at least one byte
        .iov_len = 2
    };

    union {
        char buf[CMSG_SPACE(sizeof(server_fd))];
        struct cmsghdr align;
    } u;

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = u.buf,
        .msg_controllen = sizeof(u.buf)
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    *cmsg = (struct cmsghdr){
        .cmsg_level = SOL_SOCKET,
        .cmsg_type = SCM_RIGHTS,
        .cmsg_len = CMSG_LEN(sizeof(server_fd))
    };

    memcpy(CMSG_DATA(cmsg), &server_fd, sizeof(server_fd));

    printf("sending listening socket on admin sock\n");
    return sendmsg(admin_fd, &msg, 0);
}
