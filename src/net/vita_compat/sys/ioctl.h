// ENet's unix.c includes this header, which the vitasdk newlib lacks. It calls
// ioctl only without fcntl, and the Vita build has fcntl.
#ifndef OPENCMR2_VITA_COMPAT_SYS_IOCTL_H
#define OPENCMR2_VITA_COMPAT_SYS_IOCTL_H

// The backlog of enet_socket_listen. The game listens on no TCP socket.
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

#endif
