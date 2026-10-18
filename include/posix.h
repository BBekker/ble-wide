// SPDX-License-Identifier: MIT
#pragma once
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

inline int close_on_exec(int fd)
{
    if (fd < 0) return fd;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) return fd;
    const int error = errno;
    close(fd);
    errno = error;
    return -1;
}

inline int socket_cloexec(int domain, int type, int protocol)
{
#ifdef __APPLE__
    return close_on_exec(socket(domain, type, protocol));
#else
    return socket(domain, type | SOCK_CLOEXEC, protocol);
#endif
}

inline int pipe_cloexec(int fds[2])
{
#ifdef __APPLE__
    if (pipe(fds)) return -1;
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0) return 0;
    const int error = errno;
    close(fds[0]);
    close(fds[1]);
    errno = error;
    return -1;
#else
    return pipe2(fds, O_CLOEXEC);
#endif
}
