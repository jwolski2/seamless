#include "worker.h"
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

#define ADMIN_BACKLOG 5
#define LISTEN_ADDR "0.0.0.0"
#define LISTEN_PORT 3490

// File descriptor of the socket used to transfer listening sockets.
int admin_fd;

// Sentinel value used to indicate to the master that it can exit.
int die = 0;

// Sentinel value used to indicate to the worker that it can exit.
int die_worker = 0;

// File descriptor of the socket used to handle user requests. This is currently
// unused.
int server_fd;

// Pid of the current worker process.
int worker_pid;

// Forward decls.
int request_sockets();

int create_admin_listening_socket() {
    printf("master: creating admin listening socket\n");
    int admin_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (admin_fd == -1) {
        perror("worker: failed to create admin socket");
        return -1;
    }

    struct sockaddr_un admin_sock;
    admin_sock.sun_family = AF_UNIX;
    strcpy(admin_sock.sun_path, SOCK_PATH);

    // Remove if already exists. If does not exist, continue.
    int res_unlink = unlink(admin_sock.sun_path);
    if (res_unlink == -1 && errno != ENOENT) {
        perror("worker: failed to unlink admin sock");
        return -1;
    }

    if (bind(admin_fd, (struct sockaddr *)&admin_sock, sizeof(admin_sock)) == -1) {
        perror("worker: failed to bind to admin sock");
        return -1;
    }

    if (listen(admin_fd, ADMIN_BACKLOG) == -1) {
        perror("worker: failed to listen on admin sock");
        return -1;
    }

    return admin_fd;
}

int create_server_listening_socket() {
    printf("master: creating server listening socket\n");
    int listen_socket = socket(AF_INET, SOCK_STREAM, 6);
    if (listen_socket == -1) {
        perror("master: failed to create socket");
        return -1;
    }

    struct sockaddr_in listen_addr = {
        .sin_family = 0,
        .sin_port = htons(LISTEN_PORT),
        .sin_zero = {0},
    };

    if (inet_pton(AF_INET, LISTEN_ADDR, &listen_addr.sin_addr) == -1) {
        perror("master: failed to convert IP address");
        return -1;
    }

    printf("master: binding to listening socket\n");
    if (bind(listen_socket, (const struct sockaddr *)&listen_addr, sizeof(listen_addr)) == -1) {
        perror("master: failed to bind to listening socket");
        return -1;
    }

    printf("master: listening on listening socket\n");
    if (listen(listen_socket, 2) == -1) {
        perror("master: failed to start listening\n");
        return -1;
    }

    return listen_socket;
}

void handle_sigterm(int signum) {
    printf("master: handling sigterm\n");
    // Master can exit gracefully now from its pause loop.
    die = 1;
}

void handle_sigusr2(int signum) {
    printf("master: handling sigusr2\n");

    if (request_sockets() == -1) {
        perror("worker: failed to initiate socket transfer");
        exit(EXIT_FAILURE);
    }

    // Send a usr1 to its child in order to initiate the handoff of the
    // listening socket.
    if (kill(worker_pid, SIGUSR1) == -1) {
        perror("worker: failed to send usr1 to worker");
        exit(EXIT_FAILURE);
    }
}

int request_sockets() {
    int admin_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (admin_fd == -1) {
        perror("master: failed to create admin socket");
        return -1;
    }

    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = SOCK_PATH
    };

    if (connect(admin_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("master: failed to connect to admin socket");
        // From manpage: if connect fails, consider the state of the socket as
        // unspecified. Portable applications should close the socket.
        close(admin_fd);
        return -1;
    }

    ssize_t msg_size = sizeof(MSG_GET_SOCKS);
    ssize_t bytes_sent = sendto(admin_fd, MSG_GET_SOCKS, msg_size, 0, NULL, 0);
    if (bytes_sent == -1) {
        perror("master: failed to send getsocks to worker");
        return -1;
    } else if (bytes_sent != msg_size) {
        perror("master: failed to send entire getsocks message to worker");
        return -1;
    }

    return 0;
}

int run_master() {
    printf("master: registering sigterm signal handler\n");
    struct sigaction term;
    memset(&term, 0, sizeof(struct sigaction));
    term.sa_handler = handle_sigterm;
    int res_sig = sigaction(SIGTERM, &term, NULL);
    if (res_sig == -1) {
        perror("master: failed to register sigterm handler");
        return -1;
    }

    printf("master: registering sigusr2 signal handler\n");
    struct sigaction usr2;
    memset(&usr2, 0, sizeof(struct sigaction));
    usr2.sa_handler = handle_sigusr2;
    res_sig = sigaction(SIGUSR2, &usr2, NULL);
    if (res_sig == -1) {
        perror("master: failed to register sigusr2 handler");
        return -1;
    }

    return 0;
}

int main() {
    // Create admin listening socket which'll be used to transfer listening fds.
    // The fd for this socket will be inherited by forked children.
    admin_fd = create_admin_listening_socket();
    if (admin_fd == -1) {
        perror("master: failed to create admin listeing socket");
        exit(EXIT_FAILURE);
    }

    // Create server listening socket. This'll be mostly unused. Maybe at some
    // point I'll make it to real world to validate connection draining. The fd
    // for this socket will be inherited by forked children.
    server_fd = create_server_listening_socket();
    if (server_fd == -1) {
        perror("master: failed to create server listeing socket");
        exit(EXIT_FAILURE);
    }

    pid_t forked_pid = fork();
    if (forked_pid == -1) {
        perror("master: failed to fork worker");
        exit(EXIT_FAILURE);
    } else if (forked_pid == 0) {
        if (run_worker() == -1) {
            perror("worker: failed to start");
            exit(EXIT_FAILURE);
        }
    } else {
        // Close admin listening socket. The child process has inherited it.
        if (close(admin_fd) == -1) {
            perror("master: failed to close admin listening socket");
            exit(EXIT_FAILURE);
        }

        // Close server listening socket. The child process has inherited it.
        if (close(server_fd) == -1) {
            perror("master: failed to close server listening socket");
            exit(EXIT_FAILURE);
        }

        // Save this value as a global. The master process'll need it in its
        // signal handler in order to send a signal to its child.
        worker_pid = forked_pid;

        if (run_master() == -1) {
            perror("master: failed to start");
            exit(EXIT_FAILURE);
        }

        // Hang around until a sigterm.
        printf("master: waiting until signal\n");
        while (!die) {
            pause();
        }
    }

    return 0;
}
