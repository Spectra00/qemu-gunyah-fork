#ifndef CHAR_SOCKET_H
#define CHAR_SOCKET_H

#include "io/channel-socket.h"
#include "io/net-listener.h"
#include "chardev/char.h"
#include "qom/object.h"

#define TYPE_CHARDEV_SOCKET "chardev-socket"

#define TCP_MAX_FDS 16

typedef enum {
    TCP_CHARDEV_STATE_DISCONNECTED,
    TCP_CHARDEV_STATE_CONNECTING,
    TCP_CHARDEV_STATE_CONNECTED,
} TCPChardevState;

typedef ChardevClass SocketChardevClass;

/*
 * Unix-domain stream socket, server mode only: the one shape DroidVM uses
 * for its UART and QMP sockets (path=...,server=on,wait=on|off). TCP,
 * client/reconnect, telnet, TLS and websocket were left out of this build.
 */
struct SocketChardev {
    Chardev parent;
    QIOChannel *ioc; /* Client I/O channel */
    QIOChannelSocket *sioc; /* Client master channel */
    QIONetListener *listener;
    GSource *hup_source;
    TCPChardevState state;
    int max_size;
    int *read_msgfds;
    size_t read_msgfds_num;
    int *write_msgfds;
    size_t write_msgfds_num;

    SocketAddress *addr;
};
typedef struct SocketChardev SocketChardev;

DECLARE_INSTANCE_CHECKER(SocketChardev, SOCKET_CHARDEV,
                         TYPE_CHARDEV_SOCKET)

#endif /* CHAR_SOCKET_H */
