
#include "qemu/osdep.h"
#include "chardev/char.h"
#include "io/channel-socket.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-sockets.h"

#include "chardev/char-io.h"
#include "chardev/char-socket.h"

/*
 * Socket chardev, restored from upstream char-socket.c and trimmed to what
 * DroidVM uses: a Unix-domain stream socket in server mode, with wait=on
 * (block startup until the first client connects) or wait=off. Accept,
 * disconnect, re-listen and SCM_RIGHTS fd passing are kept as upstream wrote
 * them; TCP, client/reconnect mode, telnet, TLS and websocket are not built.
 */

static void tcp_chr_change_state(SocketChardev *s, TCPChardevState state)
{
    switch (state) {
    case TCP_CHARDEV_STATE_DISCONNECTED:
        break;
    case TCP_CHARDEV_STATE_CONNECTING:
        assert(s->state == TCP_CHARDEV_STATE_DISCONNECTED);
        break;
    case TCP_CHARDEV_STATE_CONNECTED:
        assert(s->state == TCP_CHARDEV_STATE_CONNECTING);
        break;
    }
    s->state = state;
}

static void tcp_chr_accept(QIONetListener *listener,
                           QIOChannelSocket *cioc,
                           void *opaque);

static int tcp_chr_read_poll(void *opaque);
static void tcp_chr_disconnect_locked(Chardev *chr);

static int tcp_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);

    if (s->state == TCP_CHARDEV_STATE_CONNECTED) {
        int ret =  io_channel_send_full(s->ioc, buf, len,
                                        s->write_msgfds,
                                        s->write_msgfds_num);

        if (!(ret < 0 && EAGAIN == errno)
            && s->write_msgfds_num) {
            g_free(s->write_msgfds);
            s->write_msgfds = 0;
            s->write_msgfds_num = 0;
        }

        if (ret < 0 && errno != EAGAIN) {
            if (tcp_chr_read_poll(chr) <= 0) {
                tcp_chr_disconnect_locked(chr);
            } /* else let the read handler finish it properly */
        }

        return ret;
    } else {
        errno = EIO;
        return -1;
    }
}

static int tcp_chr_read_poll(void *opaque)
{
    Chardev *chr = CHARDEV(opaque);
    SocketChardev *s = SOCKET_CHARDEV(opaque);
    if (s->state != TCP_CHARDEV_STATE_CONNECTED) {
        return 0;
    }
    s->max_size = qemu_chr_be_can_write(chr);
    return s->max_size;
}

static int tcp_get_msgfds(Chardev *chr, int *fds, int num)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);

    int to_copy = (s->read_msgfds_num < num) ? s->read_msgfds_num : num;

    assert(num <= TCP_MAX_FDS);

    if (to_copy) {
        int i;

        memcpy(fds, s->read_msgfds, to_copy * sizeof(int));

        for (i = to_copy; i < s->read_msgfds_num; i++) {
            close(s->read_msgfds[i]);
        }

        g_free(s->read_msgfds);
        s->read_msgfds = 0;
        s->read_msgfds_num = 0;
    }

    return to_copy;
}

static int tcp_set_msgfds(Chardev *chr, int *fds, int num)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);

    g_free(s->write_msgfds);
    s->write_msgfds = NULL;
    s->write_msgfds_num = 0;

    if ((s->state != TCP_CHARDEV_STATE_CONNECTED) ||
        !qio_channel_has_feature(s->ioc,
                                 QIO_CHANNEL_FEATURE_FD_PASS)) {
        return -1;
    }

    if (num) {
        s->write_msgfds = g_new(int, num);
        memcpy(s->write_msgfds, fds, num * sizeof(int));
    }

    s->write_msgfds_num = num;

    return 0;
}

static ssize_t tcp_chr_recv(Chardev *chr, char *buf, size_t len)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    int ret;
    size_t i;
    int *msgfds = NULL;
    size_t msgfds_num = 0;
    Error *err = NULL;

    if (qio_channel_has_feature(s->ioc, QIO_CHANNEL_FEATURE_FD_PASS)) {
        ret = qio_channel_readv_full(s->ioc, &iov, 1,
                                     &msgfds, &msgfds_num,
                                     0, &err);
    } else {
        ret = qio_channel_readv_full(s->ioc, &iov, 1,
                                     NULL, NULL,
                                     0, &err);
    }

    if (msgfds_num) {
        for (i = 0; i < s->read_msgfds_num; i++) {
            close(s->read_msgfds[i]);
        }

        if (s->read_msgfds_num) {
            g_free(s->read_msgfds);
        }

        s->read_msgfds = msgfds;
        s->read_msgfds_num = msgfds_num;
    }

    for (i = 0; i < s->read_msgfds_num; i++) {
        int fd = s->read_msgfds[i];
        if (fd < 0) {
            continue;
        }

        qemu_socket_set_block(fd);

#ifndef MSG_CMSG_CLOEXEC
        qemu_set_cloexec(fd);
#endif
    }

    if (ret == QIO_CHANNEL_ERR_BLOCK) {
        errno = EAGAIN;
        ret = -1;
    } else if (ret == -1) {
        error_free(err);
        errno = EIO;
    }

    return ret;
}

static GSource *tcp_chr_add_watch(Chardev *chr, GIOCondition cond)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    if (!s->ioc) {
        return NULL;
    }
    return qio_channel_create_watch(s->ioc, cond);
}

static void remove_hup_source(SocketChardev *s)
{
    if (s->hup_source != NULL) {
        g_source_destroy(s->hup_source);
        g_source_unref(s->hup_source);
        s->hup_source = NULL;
    }
}

static void tcp_chr_free_connection(Chardev *chr)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    int i;

    if (s->read_msgfds_num) {
        for (i = 0; i < s->read_msgfds_num; i++) {
            close(s->read_msgfds[i]);
        }
        g_free(s->read_msgfds);
        s->read_msgfds = NULL;
        s->read_msgfds_num = 0;
    }

    remove_hup_source(s);

    tcp_set_msgfds(chr, NULL, 0);
    remove_fd_in_watch(chr);

    if (s->ioc) {
        qio_channel_close(s->ioc, NULL);
    }
    object_unref(OBJECT(s->sioc));
    s->sioc = NULL;
    object_unref(OBJECT(s->ioc));
    s->ioc = NULL;
    g_free(chr->filename);
    chr->filename = NULL;
    tcp_chr_change_state(s, TCP_CHARDEV_STATE_DISCONNECTED);
}

static void update_disconnected_filename(SocketChardev *s)
{
    Chardev *chr = CHARDEV(s);

    g_free(chr->filename);
    if (s->addr && s->addr->type == SOCKET_ADDRESS_TYPE_UNIX) {
        chr->filename = g_strdup_printf("disconnected:unix:%s,server=on",
                                        s->addr->u.q_unix.path);
    } else {
        chr->filename = g_strdup("disconnected:socket");
    }
}

static void tcp_chr_disconnect_locked(Chardev *chr)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    bool emit_close = s->state == TCP_CHARDEV_STATE_CONNECTED;

    tcp_chr_free_connection(chr);

    if (s->listener) {
        qio_net_listener_set_client_func_full(s->listener, tcp_chr_accept,
                                              chr, NULL, chr->gcontext);
    }
    update_disconnected_filename(s);
    if (emit_close) {
        qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
    }
}

static void tcp_chr_disconnect(Chardev *chr)
{
    qemu_mutex_lock(&chr->chr_write_lock);
    tcp_chr_disconnect_locked(chr);
    qemu_mutex_unlock(&chr->chr_write_lock);
}

static gboolean tcp_chr_read(QIOChannel *chan, GIOCondition cond, void *opaque)
{
    Chardev *chr = CHARDEV(opaque);
    SocketChardev *s = SOCKET_CHARDEV(opaque);
    uint8_t buf[CHR_READ_BUF_LEN];
    int len, size;

    if ((s->state != TCP_CHARDEV_STATE_CONNECTED) ||
        s->max_size <= 0) {
        return TRUE;
    }
    len = sizeof(buf);
    if (len > s->max_size) {
        len = s->max_size;
    }
    size = tcp_chr_recv(chr, (void *)buf, len);
    if (size == 0 || (size == -1 && errno != EAGAIN)) {
        tcp_chr_disconnect(chr);
    } else if (size > 0) {
        qemu_chr_be_write(chr, buf, size);
    }

    return TRUE;
}

static gboolean tcp_chr_hup(QIOChannel *channel,
                               GIOCondition cond,
                               void *opaque)
{
    Chardev *chr = CHARDEV(opaque);
    tcp_chr_disconnect(chr);
    return G_SOURCE_REMOVE;
}

static int tcp_chr_sync_read(Chardev *chr, const uint8_t *buf, int len)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    int size;
    int saved_errno;

    if (s->state != TCP_CHARDEV_STATE_CONNECTED) {
        return 0;
    }

    qio_channel_set_blocking(s->ioc, true, NULL);
    size = tcp_chr_recv(chr, (void *) buf, len);
    saved_errno = errno;
    if (s->state != TCP_CHARDEV_STATE_DISCONNECTED) {
        qio_channel_set_blocking(s->ioc, false, NULL);
    }
    if (size == 0) {
        tcp_chr_disconnect(chr);
    }

    errno = saved_errno;
    return size;
}

static char *qemu_chr_compute_filename(SocketChardev *s)
{
    struct sockaddr_storage *ss = &s->sioc->localAddr;

    if (ss->ss_family == AF_UNIX) {
        return g_strdup_printf("unix:%s,server=on",
                               ((struct sockaddr_un *)(ss))->sun_path);
    }
    return g_strdup_printf("unknown");
}

static void update_ioc_handlers(SocketChardev *s)
{
    Chardev *chr = CHARDEV(s);

    if (s->state != TCP_CHARDEV_STATE_CONNECTED) {
        return;
    }

    remove_fd_in_watch(chr);
    chr->gsource = io_add_watch_poll(chr, s->ioc,
                                     tcp_chr_read_poll,
                                     tcp_chr_read, chr,
                                     chr->gcontext);

    remove_hup_source(s);
    s->hup_source = qio_channel_create_watch(s->ioc, G_IO_HUP);
    g_source_set_priority(s->hup_source, G_PRIORITY_DEFAULT + 1);
    g_source_set_callback(s->hup_source, (GSourceFunc)tcp_chr_hup,
                          chr, NULL);
    g_source_attach(s->hup_source, chr->gcontext);
}

static void tcp_chr_connect(void *opaque)
{
    Chardev *chr = CHARDEV(opaque);
    SocketChardev *s = SOCKET_CHARDEV(opaque);

    g_free(chr->filename);
    chr->filename = qemu_chr_compute_filename(s);

    tcp_chr_change_state(s, TCP_CHARDEV_STATE_CONNECTED);
    update_ioc_handlers(s);
    qemu_chr_be_event(chr, CHR_EVENT_OPENED);
}

static void tcp_chr_update_read_handler(Chardev *chr)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);

    if (s->listener && s->state == TCP_CHARDEV_STATE_DISCONNECTED) {
        qio_net_listener_set_client_func_full(s->listener, tcp_chr_accept,
                                              chr, NULL, chr->gcontext);
    }

    update_ioc_handlers(s);
}

static void tcp_chr_set_client_ioc_name(Chardev *chr,
                                        QIOChannelSocket *sioc)
{
    char *name;
    name = g_strdup_printf("chardev-unix-server-%s", chr->label);
    qio_channel_set_name(QIO_CHANNEL(sioc), name);
    g_free(name);
}

static int tcp_chr_new_client(Chardev *chr, QIOChannelSocket *sioc)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);

    if (s->state != TCP_CHARDEV_STATE_CONNECTING) {
        return -1;
    }

    s->ioc = QIO_CHANNEL(sioc);
    object_ref(OBJECT(sioc));
    s->sioc = sioc;
    object_ref(OBJECT(sioc));

    qio_channel_set_blocking(s->ioc, false, NULL);

    if (s->listener) {
        qio_net_listener_set_client_func_full(s->listener, NULL, NULL,
                                              NULL, chr->gcontext);
    }

    tcp_chr_connect(chr);

    return 0;
}

static void tcp_chr_accept(QIONetListener *listener,
                           QIOChannelSocket *cioc,
                           void *opaque)
{
    Chardev *chr = CHARDEV(opaque);
    SocketChardev *s = SOCKET_CHARDEV(chr);

    tcp_chr_change_state(s, TCP_CHARDEV_STATE_CONNECTING);
    tcp_chr_set_client_ioc_name(chr, cioc);
    tcp_chr_new_client(chr, cioc);
}

static void tcp_chr_accept_server_sync(Chardev *chr)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    QIOChannelSocket *sioc;
    info_report("QEMU waiting for connection on: %s",
                chr->filename);
    tcp_chr_change_state(s, TCP_CHARDEV_STATE_CONNECTING);
    sioc = qio_net_listener_wait_client(s->listener);
    tcp_chr_set_client_ioc_name(chr, sioc);
    tcp_chr_new_client(chr, sioc);
    object_unref(OBJECT(sioc));
}

static int tcp_chr_wait_connected(Chardev *chr, Error **errp)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);

    while (s->state != TCP_CHARDEV_STATE_CONNECTED) {
        tcp_chr_accept_server_sync(chr);
    }

    return 0;
}

static void char_socket_finalize(Object *obj)
{
    Chardev *chr = CHARDEV(obj);
    SocketChardev *s = SOCKET_CHARDEV(obj);

    tcp_chr_free_connection(chr);
    qapi_free_SocketAddress(s->addr);
    if (s->listener) {
        qio_net_listener_set_client_func_full(s->listener, NULL, NULL,
                                              NULL, chr->gcontext);
        object_unref(OBJECT(s->listener));
        s->listener = NULL;
    }

    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

static int qmp_chardev_open_socket_server(Chardev *chr,
                                          bool is_waitconnect,
                                          Error **errp)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    char *name;

    s->listener = qio_net_listener_new();

    name = g_strdup_printf("chardev-unix-listener-%s", chr->label);
    qio_net_listener_set_name(s->listener, name);
    g_free(name);

    if (qio_net_listener_open_sync(s->listener, s->addr, 1, errp) < 0) {
        object_unref(OBJECT(s->listener));
        s->listener = NULL;
        return -1;
    }

    update_disconnected_filename(s);

    if (is_waitconnect) {
        tcp_chr_accept_server_sync(chr);
    } else {
        qio_net_listener_set_client_func_full(s->listener,
                                              tcp_chr_accept,
                                              chr, NULL,
                                              chr->gcontext);
    }

    return 0;
}

static void qmp_chardev_open_socket(Chardev *chr,
                                    ChardevBackend *backend,
                                    bool *be_opened,
                                    Error **errp)
{
    SocketChardev *s = SOCKET_CHARDEV(chr);
    ChardevSocket *sock = backend->u.socket.data;
    bool is_listen      = sock->has_server  ? sock->server  : true;
    bool is_waitconnect = sock->has_wait    ? sock->wait    : false;
    SocketAddress *addr;

    if (!is_listen) {
        error_setg(errp, "chardev: socket: only server=on is supported "
                   "in this build");
        return;
    }

    s->addr = addr = socket_address_flatten(sock->addr);
    if (addr->type != SOCKET_ADDRESS_TYPE_UNIX) {
        error_setg(errp, "chardev: socket: only unix 'path' sockets are "
                   "supported in this build");
        return;
    }

    qemu_chr_set_feature(chr, QEMU_CHAR_FEATURE_RECONNECTABLE);
    qemu_chr_set_feature(chr, QEMU_CHAR_FEATURE_FD_PASS);

    *be_opened = false;

    update_disconnected_filename(s);

    qmp_chardev_open_socket_server(chr, is_waitconnect, errp);
}

static void qemu_chr_parse_socket(QemuOpts *opts, ChardevBackend *backend,
                                  Error **errp)
{
    const char *path = qemu_opt_get(opts, "path");
    SocketAddressLegacy *addr;
    ChardevSocket *sock;
    UnixSocketAddress *q_unix;

    if (!path) {
        error_setg(errp, "chardev: socket: 'path' option required "
                   "(only unix sockets are supported in this build)");
        return;
    }

    backend->type = CHARDEV_BACKEND_KIND_SOCKET;
    sock = backend->u.socket.data = g_new0(ChardevSocket, 1);
    qemu_chr_parse_common(opts, qapi_ChardevSocket_base(sock));

    sock->has_server = true;
    sock->server = qemu_opt_get_bool(opts, "server", false);
    sock->has_wait = qemu_opt_find(opts, "wait") || sock->server;
    sock->wait = qemu_opt_get_bool(opts, "wait", true);

    addr = g_new0(SocketAddressLegacy, 1);
    addr->type = SOCKET_ADDRESS_TYPE_UNIX;
    q_unix = addr->u.q_unix.data = g_new0(UnixSocketAddress, 1);
    q_unix->path = g_strdup(path);
    sock->addr = addr;
}

static bool
char_socket_get_connected(Object *obj, Error **errp)
{
    SocketChardev *s = SOCKET_CHARDEV(obj);

    return s->state == TCP_CHARDEV_STATE_CONNECTED;
}

static void char_socket_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->parse = qemu_chr_parse_socket;
    cc->open = qmp_chardev_open_socket;
    cc->chr_wait_connected = tcp_chr_wait_connected;
    cc->chr_write = tcp_chr_write;
    cc->chr_sync_read = tcp_chr_sync_read;
    cc->chr_disconnect = tcp_chr_disconnect;
    cc->get_msgfds = tcp_get_msgfds;
    cc->set_msgfds = tcp_set_msgfds;
    cc->chr_add_watch = tcp_chr_add_watch;
    cc->chr_update_read_handler = tcp_chr_update_read_handler;

    object_class_property_add_bool(oc, "connected", char_socket_get_connected,
                                   NULL);
}

static const TypeInfo char_socket_type_info = {
    .name = TYPE_CHARDEV_SOCKET,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(SocketChardev),
    .instance_finalize = char_socket_finalize,
    .class_init = char_socket_class_init,
};

static void register_types(void)
{
    type_register_static(&char_socket_type_info);
}

type_init(register_types);
