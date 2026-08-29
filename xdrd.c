/*
 *  xdrd 1.0-git
 *  Copyright (C) 2013-2023  Konrad Kosmatka
 *  http://fmdx.pl/

 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include "xdr-protocol.h"

#include <termios.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#define DEFAULT_SERIAL "/dev/ttyUSB0"

#ifdef __APPLE__
/* accept4 and SOCK_CLOEXEC are Linux-specific; emulate on macOS for builds */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
static int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    (void)flags;
    int fd = accept(sockfd, addr, addrlen);
    if(fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
#endif

#define VERSION       "1.0-git"
#define DEFAULT_USERS 10
#define SERIAL_BUFFER 8192
#define WS_GUID       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct user
{
    int fd;
    int auth;
    int ws;
    struct user* next;
    struct user* prev;
} user_t;

typedef struct server
{
    int serialfd;
    pthread_mutex_t mutex; // users mutex
    pthread_mutex_t mutex_s; // serial mutex
    int background; // run in background
    int guest; // allow users without auth
    char* password; // server password
    int maxusers; // number of allowed users at the same time
    int poweroff; // power tuner off when nobody is connected

    char* f_exec; // command to run after first user has connected
    char* l_exec; // command to run after last user has disconnected

    int online; // online users counter
    int online_auth;

    // tuner settings
    int mode;
    int volume;
    int freq;
    int deemphasis;
    int agc;
    int filter;
    int bandwidth;
    int ant;
    int gain;
    int daa;
    int squelch;
    int rotator;
    int sampling;
    int detector;

    user_t* head;
} server_t;

typedef struct thread
{
    int fd;
    char* ip;
    uint16_t port;
} thread_t;

typedef struct listener
{
    int fd;
    void* (*conn_handler)(void*);
} listener_t;

server_t server;

void show_usage(char*);
void server_log(int prio, char* msg, ...);
char* prepare_cmd(const char*);
void server_init(int, void* (*)(void*));
void* server_thread(void*);
void* server_conn(void*);
void* ws_server_conn(void*);
int  ws_handshake(int);
int  ws_read_frame(int, char*, int);
int  ws_write_frame(int, const char*, int);
static int client_auth(int, int);
void serial_init(char*);
void serial_loop(void);
void serial_write(char*, int);
user_t* user_add(server_t*, int, int, int);
void user_remove(server_t*, user_t*);
void msg_parse_serial(char, char*);
void msg_send(char*, int);
char* auth_salt(void);
int auth_hash(char*, char*, char*);
void tuner_defaults(void);
void tuner_reset(void);
void socket_close(int);

#ifndef XDRD_NO_MAIN
int main(int argc, char* argv[])
{
    char serial[250] = DEFAULT_SERIAL;
    int port = XDR_TCP_DEFAULT_PORT;
    int ws_port = 0;
    int c;

    server.background = 0;
    server.guest = 0;
    server.password = NULL;
    server.maxusers = DEFAULT_USERS;
    server.f_exec = NULL;
    server.l_exec = NULL;
    server.online = 0;
    server.online_auth = 0;
    server.head = NULL;
    tuner_defaults();
    pthread_mutex_init(&server.mutex, NULL);
    pthread_mutex_init(&server.mutex_s, NULL);

    if(getuid() == 0)
    {
        fprintf(stderr, "error: running the server as root is a bad idea, giving up!\n");
        exit(EXIT_FAILURE);
    }

    while((c = getopt(argc, argv, "hbgxt:s:u:p:f:l:w:")) != -1)
    {
        switch(c)
        {
        case 'h':
            show_usage(argv[0]);

        case 'b':
            server.background = 1;
            break;

        case 'g':
            server.guest = 1;
            break;

        case 'x':
            server.poweroff = 1;
            break;

        case 't':
            port = atoi(optarg);
            break;

        case 's':
            snprintf(serial, sizeof(serial), "%s", optarg);
            break;

        case 'u':
            server.maxusers = atoi(optarg);
            break;

        case 'p':
            server.password = optarg;
            break;

        case 'f':
            server.f_exec = prepare_cmd(optarg);
            break;

        case 'l':
            server.l_exec = prepare_cmd(optarg);
            break;

        case 'w':
            ws_port = atoi(optarg);
            break;

        case ':':
        case '?':
            show_usage(argv[0]);
        }
    }

    if(port < 1024 || port > 65535)
    {
        fprintf(stderr, "error: the tcp port must be in 1024-65535 range\n");
        show_usage(argv[0]);
    }

    if(ws_port && (ws_port < 1024 || ws_port > 65535))
    {
        fprintf(stderr, "error: the websocket port must be in 1024-65535 range\n");
        show_usage(argv[0]);
    }

    if(ws_port && ws_port == port)
    {
        fprintf(stderr, "error: tcp port and websocket port must be different\n");
        show_usage(argv[0]);
    }

    if(!server.password || !strlen(server.password))
    {
        fprintf(stderr, "error: no password specified\n");
        show_usage(argv[0]);
    }

    if(server.background)
    {
        switch(fork())
        {
        case -1:
            server_log(LOG_ERR, "fork");
            exit(EXIT_FAILURE);

        case 0:
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            umask(0);
            break;

        default:
            exit(EXIT_SUCCESS);
        }

        if(open("/dev/null", O_RDONLY) == -1 ||
           open("/dev/null", O_WRONLY) == -1 ||
           open("/dev/null", O_RDWR) == -1)
        {
            server_log(LOG_ERR, "open /dev/null");
            exit(EXIT_FAILURE);
        }

        if(setsid() < 0)
        {
            server_log(LOG_ERR, "setsid");
            exit(EXIT_FAILURE);
        }

        if(chdir("/") < 0)
        {
            server_log(LOG_ERR, "chdir");
            exit(EXIT_FAILURE);
        }
    }

    server_log(LOG_INFO, "xdrd " VERSION " is starting using %s and TCP port: %d", serial, port);
    server_init(port, server_conn);

    if(ws_port)
    {
        server_log(LOG_INFO, "xdrd WebSocket server starting on port: %d", ws_port);
        server_init(ws_port, ws_server_conn);
    }

    serial_init(serial);
    serial_loop();
    server_log(LOG_ERR, "lost connection with tuner");
    return EXIT_FAILURE;
}
#endif /* XDRD_NO_MAIN */

void show_usage(char* arg)
{
    printf("xdrd " VERSION "\n");
    printf("usage:\n");
    printf("%s [ -s serial ] [ -t port ] [ -w wsport ] [ -u users ]\n", arg);
    printf("%*s [ -p password ] [ -f command ] [ -l command ]\n", (int)strlen(arg), "");
    printf("%*s [ -hgxb ]\n", (int)strlen(arg), "");
    printf("options:\n");
    printf("  -s  serial port (default %s)\n", DEFAULT_SERIAL);
    printf("  -t  tcp/ip port (default %d)\n", XDR_TCP_DEFAULT_PORT);
    printf("  -w  websocket port (disabled by default)\n");
    printf("  -u  max users   (default %d)\n", DEFAULT_USERS);
    printf("  -p  specify password (required)\n");
    printf("  -h  show this help list\n");
    printf("  -g  allow guest login (read-only access)\n");
    printf("  -x  power the tuner off after last user has disconnected\n");
    printf("  -f  execute the specified command after first user has connected\n");
    printf("  -l  execute the specified command after last user has disconnected\n");
    printf("  -b  run server in the background\n");
    exit(EXIT_SUCCESS);
}

void server_log(int prio, char* msg, ...)
{
    va_list myargs;
    va_start(myargs, msg);
    if(server.background)
    {
        vsyslog(prio, msg, myargs);
    }
    else
    {
        switch(prio)
        {
        case LOG_ERR:
            fprintf(stderr, "error: ");
            vfprintf(stderr, msg, myargs);
            fprintf(stderr, "\n");
            break;
        default:
            vfprintf(stdout, msg, myargs);
            fprintf(stdout, "\n");
            break;
        }
    }
    va_end(myargs);
}

char* prepare_cmd(const char* cmd)
{
    char* buff;
    int len;
    len = strlen(cmd) + 1 + 1;
    buff = malloc(len);
    memcpy(buff, cmd, strlen(cmd));
    buff[len-1] = '&';
    buff[len] = '\0';
    return buff;
}

void server_init(int port, void* (*conn_handler)(void*))
{
    int sockfd;
    struct sockaddr_in addr;
    pthread_t thread;
    listener_t* l;

    if((sockfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
    {
        server_log(LOG_ERR, "server_init: socket");
        exit(EXIT_FAILURE);
    }

    int value = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&value, sizeof(value)) < 0)
    {
        server_log(LOG_ERR, "server_init: SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

    memset((char*)&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        server_log(LOG_ERR, "server_init: bind");
        exit(EXIT_FAILURE);
    }

    listen(sockfd, 4);

    l = malloc(sizeof(listener_t));
    l->fd = sockfd;
    l->conn_handler = conn_handler;

    if(pthread_create(&thread, NULL, server_thread, (void*)l))
    {
        server_log(LOG_ERR, "server_init: pthread_create");
        exit(EXIT_FAILURE);
    }
}

void* server_thread(void* arg)
{
    listener_t* l = (listener_t*)arg;
    pthread_t thread;
    pthread_attr_t attr;
    int connfd;
    thread_t *t_data;
    struct sockaddr_in dest;
    socklen_t dest_size = sizeof(struct sockaddr_in);

    if(pthread_attr_init(&attr))
    {
        server_log(LOG_ERR, "server_thread: pthread_attr_init");
        exit(EXIT_FAILURE);
    }

    if(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
    {
        server_log(LOG_ERR, "server_thread: pthread_attr_setdetachstate");
        exit(EXIT_FAILURE);
    }

    while((connfd = accept4(l->fd, (struct sockaddr *)&dest, &dest_size, SOCK_CLOEXEC)) >= 0)
    {
        if(server.online >= server.maxusers)
        {
            socket_close(connfd);
            continue;
        }

        t_data = malloc(sizeof(thread_t));
        t_data->fd = connfd;
        t_data->ip = strdup(inet_ntoa(dest.sin_addr));
        t_data->port = ntohs(dest.sin_port);
        if(pthread_create(&thread, &attr, l->conn_handler, (void*)t_data))
        {
            server_log(LOG_ERR, "server_thread: pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    pthread_attr_destroy(&attr);
    server_log(LOG_ERR, "server_thread: accept");
    exit(EXIT_FAILURE);
    return NULL;
}

/*
 * Challenge-response auth shared by both transports (ws selects the framing).
 * Returns 1 = authenticated, 0 = connected as guest, -1 = rejected.
 */
static int client_auth(int fd, int ws)
{
    char buffer[128];
    int auth = 0;
    ssize_t n = 0;
    char* salt;

    if(!(salt = auth_salt()))
        return -1;

    snprintf(buffer, sizeof(buffer), "%s\n", salt);
    if(ws)
    {
        ws_write_frame(fd, buffer, strlen(buffer));
        n = ws_read_frame(fd, buffer, sizeof(buffer)-1);
    }
    else
    {
        send(fd, buffer, strlen(buffer), MSG_NOSIGNAL);
        n = recv(fd, buffer, 41, MSG_NOSIGNAL);
    }

    if(n >= 40)
    {
        buffer[40] = 0;
        auth = auth_hash(salt, server.password, buffer);
    }

    free(salt);

    if(!auth && !server.guest)
    {
        if(ws)
            ws_write_frame(fd, "a0\n", 3);
        else
            send(fd, "a0\n", 3, MSG_NOSIGNAL);
        return -1;
    }

    if(!auth)
    {
        if(ws)
            ws_write_frame(fd, "a1\n", 3);
        else
            send(fd, "a1\n", 3, MSG_NOSIGNAL);
    }

    return auth;
}

void* server_conn(void* t_data)
{
    int connfd = ((thread_t*)t_data)->fd;
    char* ip = ((thread_t*)t_data)->ip;
    uint16_t port = ((thread_t*)t_data)->port;

    user_t *u;
    fd_set input;
    char buffer[100];
    int pos = 0, auth;

    free(t_data);

    if((auth = client_auth(connfd, 0)) < 0)
    {
        socket_close(connfd);
        free(ip);
        return NULL;
    }

    fcntl(connfd, F_SETFL, O_NONBLOCK);

    server_log(LOG_INFO, "user connected: %s:%u%s", ip, port, (auth ? "" : " (guest)"));

    u = user_add(&server, connfd, auth, 0);

    FD_ZERO(&input);
    FD_SET(u->fd, &input);
    while(select(u->fd+1, &input, NULL, NULL, NULL) > 0)
    {
        if(recv(u->fd, &buffer[pos], 1, MSG_NOSIGNAL) <= 0)
            break;

        /* If this command is too long to
         * fit into a buffer, clip it */
        if(buffer[pos] != '\n')
        {
            if(pos != sizeof(buffer)-1)
                pos++;
            continue;
        }

        if(buffer[0] == XDR_P_SHUTDOWN)
            break;

        if(u->auth)
            serial_write(buffer, pos+1);

        pos = 0;
    }

    user_remove(&server, u);
    server_log(LOG_INFO, "user disconnected: %s:%u", ip, port);
    free(ip);

    return NULL;
}

/* ── WebSocket server ─────────────────────────────────────────────────────── */

void* ws_server_conn(void* t_data)
{
    int connfd = ((thread_t*)t_data)->fd;
    char* ip = ((thread_t*)t_data)->ip;
    uint16_t port = ((thread_t*)t_data)->port;

    char frame[256];
    int auth, n;
    user_t *u;

    free(t_data);

    if(!ws_handshake(connfd))
    {
        socket_close(connfd);
        free(ip);
        return NULL;
    }

    if((auth = client_auth(connfd, 1)) < 0)
    {
        socket_close(connfd);
        free(ip);
        return NULL;
    }

    /* Non-blocking like the TCP path, so a stalled client cannot block
     * msg_send while it holds the users mutex. */
    fcntl(connfd, F_SETFL, O_NONBLOCK);

    server_log(LOG_INFO, "WS user connected: %s:%u%s", ip, port, (auth ? "" : " (guest)"));

    u = user_add(&server, connfd, auth, 1);

    while(1)
    {
        n = ws_read_frame(connfd, frame, sizeof(frame) - 1);
        if(n <= 0)
            break;

        if(frame[0] == XDR_P_SHUTDOWN)
            break;

        if(u->auth)
        {
            /* Ensure newline terminator for serial_write */
            if(frame[n-1] != '\n')
                frame[n++] = '\n';
            serial_write(frame, n);
        }
    }

    user_remove(&server, u);
    server_log(LOG_INFO, "WS user disconnected: %s:%u", ip, port);
    free(ip);
    return NULL;
}

/*
 * Performs the RFC 6455 WebSocket opening handshake.
 * Reads HTTP headers, computes Sec-WebSocket-Accept, sends 101.
 * Returns 1 on success, 0 on failure.
 */
int ws_handshake(int fd)
{
    char buf[4096];
    char key[256];
    int total = 0;
    ssize_t n;
    char *p, *end;

    memset(buf, 0, sizeof(buf));

    /* Read HTTP request until we see the blank line */
    while(total < (int)sizeof(buf) - 1)
    {
        n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if(n <= 0)
            return 0;
        total += n;
        buf[total] = 0;
        if(strstr(buf, "\r\n\r\n"))
            break;
        if(total >= (int)sizeof(buf) - 1)
            return 0;
    }

    /* Require an "Upgrade: websocket" header; otherwise this is a plain
     * HTTP request and must not be answered with 101. */
    p = strcasestr(buf, "Upgrade:");
    if(!p)
        return 0;
    end = strstr(p, "\r\n");
    if(!end)
        return 0;
    *end = 0;
    if(!strcasestr(p, "websocket"))
        return 0;
    *end = '\r';

    p = strcasestr(buf, "Sec-WebSocket-Key:");
    if(!p)
        return 0;
    p += 18;
    while(*p == ' ')
        p++;
    end = strstr(p, "\r\n");
    if(!end)
        return 0;
    if((end - p) >= (int)sizeof(key))
        return 0;
    memcpy(key, p, end - p);
    key[end - p] = 0;

    /* Sec-WebSocket-Accept = base64(SHA1(key + GUID)) */
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);

    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)combined, strlen(combined), sha);

    unsigned char accept_b64[64];
    int olen = EVP_EncodeBlock(accept_b64, sha, SHA_DIGEST_LENGTH);
    accept_b64[olen] = 0;

    char response[512];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_b64);

    send(fd, response, strlen(response), MSG_NOSIGNAL);
    return 1;
}

/* Read exactly len bytes from fd into buf, waiting in select() when the fd
 * is non-blocking. Returns 1 on success, 0 on error/close. */
static int recv_all(int fd, void* buf, size_t len)
{
    size_t total = 0;
    fd_set input;
    while(total < len)
    {
        ssize_t n = recv(fd, (char*)buf + total, len - total, 0);
        if(n > 0)
        {
            total += n;
            continue;
        }
        if(n < 0 && errno == EINTR)
            continue;
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            FD_ZERO(&input);
            FD_SET(fd, &input);
            select(fd+1, &input, NULL, NULL, NULL);
            continue;
        }
        return 0;
    }
    return 1;
}

/* ponytail: one global write lock for all WS fds; per-user locks if throughput matters */
static pthread_mutex_t ws_write_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Send one fully-assembled WS frame. Serialized so the broadcast thread and a
 * connection thread's pong cannot interleave bytes on the same fd.
 * Returns 0 on success, -1 on error (caller should drop the client). */
static int ws_send_locked(int fd, const unsigned char* data, int len)
{
    int sent = 0, n, ret = 0;

    pthread_mutex_lock(&ws_write_mutex);
    while(sent < len)
    {
        n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if(n < 0)
        {
            ret = -1;
            break;
        }
        sent += n;
    }
    pthread_mutex_unlock(&ws_write_mutex);
    return ret;
}

/*
 * Reads one complete WebSocket message from fd into buf (null-terminated),
 * reassembling fragmented messages (FIN=0 + continuation frames).
 * Handles masking (client→server frames are always masked per RFC 6455).
 * Transparently handles ping/pong; empty messages are skipped.
 * Returns payload length on success, 0 on clean close/disconnect, -1 on error.
 */
int ws_read_frame(int fd, char* buf, int maxlen)
{
    unsigned char hdr[2];
    uint64_t plen;
    unsigned char mask[4];
    int masked, opcode, fin;
    int off = 0;
    int i;

    while(1)
    {
        if(!recv_all(fd, hdr, 2))
            return 0;

        fin = (hdr[0] >> 7) & 1;
        opcode = hdr[0] & 0x0F;
        masked = (hdr[1] >> 7) & 1;
        plen = hdr[1] & 0x7F;

        if(plen == 126)
        {
            unsigned char ext[2];
            if(!recv_all(fd, ext, 2))
                return 0;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        }
        else if(plen == 127)
        {
            unsigned char ext[8];
            if(!recv_all(fd, ext, 8))
                return 0;
            plen = 0;
            for(i = 0; i < 8; i++)
                plen = (plen << 8) | ext[i];
        }

        memset(mask, 0, sizeof(mask));
        if(masked && !recv_all(fd, mask, 4))
            return 0;

        if(opcode & 0x8) /* Control frame: close/ping/pong */
        {
            unsigned char payload[125];

            /* RFC 6455 §5.5: control frames carry at most 125 bytes;
             * anything larger cannot be resynced — drop the connection. */
            if(plen > 125)
                return -1;

            if(plen > 0 && !recv_all(fd, payload, (size_t)plen))
                return 0;
            if(masked)
                for(i = 0; i < (int)plen; i++)
                    payload[i] ^= mask[i % 4];

            if(opcode == 0x8) /* Close */
                return 0;

            if(opcode == 0x9) /* Ping — respond with Pong */
            {
                unsigned char pong[2 + 125];
                pong[0] = 0x8A;
                pong[1] = (unsigned char)plen;
                memcpy(pong + 2, payload, (size_t)plen);
                ws_send_locked(fd, pong, 2 + (int)plen);
            }

            /* Pong (0xA) and reserved control opcodes: ignore */
            continue;
        }

        /* Data frame: text (0x1), binary (0x2) or continuation (0x0) */
        if((off == 0 && opcode == 0x0) || opcode > 0x2)
            return -1; /* stray continuation or reserved opcode */

        if(plen >= (uint64_t)(maxlen - off))
            return -1;

        if(plen > 0 && !recv_all(fd, buf + off, (size_t)plen))
            return 0;

        if(masked)
            for(i = 0; i < (int)plen; i++)
                buf[off + i] ^= mask[i % 4];

        off += (int)plen;

        if(!fin)
            continue; /* accumulate fragments until FIN */

        if(off == 0)
            continue; /* empty message (legal): skip, keep reading */

        buf[off] = 0;
        return off;
    }
}

/*
 * Sends data as a single unmasked WebSocket text frame (server→client frames
 * are never masked per RFC 6455). Returns 0 on success, -1 on error.
 */
int ws_write_frame(int fd, const char* data, int len)
{
    unsigned char frame[SERIAL_BUFFER + 4];
    int hlen;

    if(len < 0 || len > SERIAL_BUFFER)
        return -1;

    frame[0] = 0x81; /* FIN=1, opcode=text */

    if(len < 126)
    {
        frame[1] = (unsigned char)len;
        hlen = 2;
    }
    else
    {
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        hlen = 4;
    }

    memcpy(frame + hlen, data, len);
    return ws_send_locked(fd, frame, hlen + len);
}

/* ── Serial ───────────────────────────────────────────────────────────────── */

void serial_init(char* path)
{
    if((server.serialfd = open(path, O_RDWR | O_NOCTTY | O_NDELAY | O_CLOEXEC)) < 0)
    {
        server_log(LOG_ERR, "serial_init: open");
        exit(EXIT_FAILURE);
    }

    fcntl(server.serialfd, F_SETFL, 0);
    tcflush(server.serialfd, TCIOFLUSH);

    struct termios options;
    if(tcgetattr(server.serialfd, &options))
    {
        close(server.serialfd);
        server_log(LOG_ERR, "serial_init: tcgetattr");
        exit(EXIT_FAILURE);
    }

    if(cfsetispeed(&options, B115200) || cfsetospeed(&options, B115200))
    {
        close(server.serialfd);
        server_log(LOG_ERR, "serial_init: cfsetspeed");
        exit(EXIT_FAILURE);
    }

    options.c_iflag &= ~(BRKINT | ICRNL | IXON | IMAXBEL);
    options.c_iflag |= IGNBRK;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN | ECHOK | ECHOCTL | ECHOKE);
    options.c_oflag &= ~(OPOST | ONLCR);
    options.c_oflag |= NOFLSH;
    options.c_cflag |= CS8;
    options.c_cflag &= ~(CRTSCTS);
    if(tcsetattr(server.serialfd, TCSANOW, &options))
    {
        close(server.serialfd);
        server_log(LOG_ERR, "serial_init: tcsetattr");
        exit(EXIT_FAILURE);
    }

    tuner_reset();
}

void serial_loop(void)
{
    char buff[SERIAL_BUFFER];
    int pos = 0;

    fd_set input;
    FD_ZERO(&input);
    FD_SET(server.serialfd, &input);
    while(select(server.serialfd+1, &input, NULL, NULL, NULL) > 0)
    {
        if(read(server.serialfd, &buff[pos], 1) <= 0)
            break;

        if(buff[pos] != '\n') /* If this command is too long to fit into a buffer, clip it */
        {
            if(pos != SERIAL_BUFFER-1)
                pos++;
            continue;
        }
        buff[pos] = 0;
        if(pos)
            msg_parse_serial(buff[0], buff+1);
        buff[pos] = '\n';
        msg_send(buff, pos+1);
        pos = 0;
    }

    close(server.serialfd);
}

void serial_write(char* msg, int len)
{
    pthread_mutex_lock(&server.mutex_s);
    ssize_t unused = write(server.serialfd, msg, len);
    (void) unused;
    pthread_mutex_unlock(&server.mutex_s);
}

/* ── User management ──────────────────────────────────────────────────────── */

user_t* user_add(server_t* LIST, int fd, int auth, int ws)
{
    user_t* u = malloc(sizeof(user_t));
    u->fd = fd;
    u->auth = auth;
    u->ws = ws;
    u->prev = NULL;

    pthread_mutex_lock(&LIST->mutex);
    u->next = LIST->head;
    if(LIST->head)
    {
        (LIST->head)->prev = u;
    }
    LIST->head = u;
    LIST->online++;
    LIST->online_auth += auth;

    if(server.f_exec && LIST->online_auth == 1)
    {
        server_log(LOG_INFO, "executing: %s", server.f_exec);
        ssize_t unused = system(server.f_exec);
        (void) unused;
    }

    pthread_mutex_unlock(&LIST->mutex);
    return u;
}

void user_remove(server_t* LIST, user_t* USER)
{
    pthread_mutex_lock(&LIST->mutex);
    if(USER->prev)
    {
        (USER->prev)->next = USER->next;
    }
    else
    {
        LIST->head = USER->next;
    }
    if(USER->next)
    {
        (USER->next)->prev = USER->prev;
    }
    LIST->online--;
    LIST->online_auth -= USER->auth;

    if(server.l_exec && LIST->online_auth == 0)
    {
        server_log(LOG_INFO, "executing: %s", server.l_exec);
        ssize_t unused = system(server.l_exec);
        (void) unused;
    }

    pthread_mutex_unlock(&LIST->mutex);
    socket_close(USER->fd);
    free(USER);
}

/* ── Message broadcast ────────────────────────────────────────────────────── */

void msg_parse_serial(char cmd, char* msg)
{
    char *ptr;
    switch(cmd)
    {
    case XDR_P_SHUTDOWN:
        tuner_defaults();
        return;

    case XDR_P_MODE:
        server.mode = atoi(msg);
        server.filter = XDR_P_FILTER_DEFAULT;

        if (server.bandwidth != XDR_P_BANDWIDTH_INVALID)
        {
            server.bandwidth = XDR_P_BANDWIDTH_DEFAULT;
        }
        else
        {
            server.filter = XDR_P_FILTER_DEFAULT;
        }

        return;

    case XDR_P_TUNE:
        server.freq = atoi(msg);
        return;

    case XDR_P_FILTER:
        server.filter = atoi(msg);
        server.bandwidth = XDR_P_BANDWIDTH_INVALID;
        return;

    case XDR_P_BANDWIDTH:
        server.bandwidth = atoi(msg);
        return;

    case XDR_P_DAA:
        server.daa = atoi(msg);
        return;

    case XDR_P_DEEMPHASIS:
        server.deemphasis = atoi(msg);
        return;

    case XDR_P_AGC:
        server.agc = atoi(msg);
        return;

    case XDR_P_GAIN:
        server.gain = atoi(msg);
        return;

    case XDR_P_SQUELCH:
        server.squelch = atoi(msg);
        return;

    case XDR_P_VOLUME:
        server.volume = atoi(msg);
        return;

    case XDR_P_ANTENNA:
        server.ant = atoi(msg);
        return;

    case XDR_P_ROTATOR:
        server.rotator = atoi(msg);
        return;

    case XDR_P_INTERVAL:
        server.sampling = atoi(msg);
        for(ptr = msg; *ptr != '\0'; ptr++)
        {
            if(*ptr == ',')
            {
                server.detector = (*(ptr+1) == '1');
                break;
            }
        }
        return;
    }
}

void msg_send(char* msg, int len)
{
    int sent, n;
    user_t *u;

    pthread_mutex_lock(&server.mutex);
    for(u = server.head; u; u=u->next)
    {
        if(server.guest || u->auth)
        {
            if(u->ws)
            {
                if(ws_write_frame(u->fd, msg, len) < 0)
                    shutdown(u->fd, 2);
            }
            else
            {
                sent = 0;
                do
                {
                    n = send(u->fd, msg+sent, len-sent, MSG_NOSIGNAL);
                    if(n < 0)
                    {
                        shutdown(u->fd, 2);
                        break;
                    }
                    sent += n;
                }
                while(sent<len);
            }
        }
    }
    pthread_mutex_unlock(&server.mutex);
}

/* ── Auth ─────────────────────────────────────────────────────────────────── */

char* auth_salt(void)
{
    static const char chars[] = "QWERTYUIOPASDFGHJKLZXCVBNMqwertyuiopasdfghjklzxcvbnm0123456789_-";
    const int len = strlen(chars);
    unsigned char random_data[XDR_TCP_SALT_LENGTH];
    char* output;
    int i;

    if(!RAND_bytes(random_data, sizeof(random_data)))
    {
        server_log(LOG_ERR, "RAND_bytes failed!");
        return NULL;
    }

    output = (char*)malloc(sizeof(char)*(XDR_TCP_SALT_LENGTH+1));
    for(i=0; i<XDR_TCP_SALT_LENGTH; i++)
    {
        output[i] = chars[random_data[i]%len];
    }
    output[i] = 0;
    return output;
}

int auth_hash(char* salt, char* password, char* hash)
{
    SHA_CTX ctx;
    unsigned char sha[SHA_DIGEST_LENGTH];
    char sha_string[SHA_DIGEST_LENGTH*2+1];
    int i;

    SHA1_Init(&ctx);
    SHA1_Update(&ctx, (unsigned char*)salt, strlen(salt));
    SHA1_Update(&ctx, (unsigned char*)password, strlen(password));
    SHA1_Final(sha, &ctx);

    for(i=0; i<SHA_DIGEST_LENGTH; i++)
        sprintf(sha_string+(i*2), "%02x", sha[i]);

    return (strcasecmp(hash, sha_string) == 0);
}

/* ── Tuner state ──────────────────────────────────────────────────────────── */

void tuner_defaults(void)
{
    server.mode = XDR_P_MODE_DEFAULT;
    server.volume = XDR_P_VOLUME_DEFAULT;
    server.freq = XDR_P_TUNE_DEFAULT;
    server.deemphasis = XDR_P_DEEMPHASIS_DEFAULT;
    server.agc = XDR_P_AGC_DEFAULT;
    server.filter = XDR_P_FILTER_DEFAULT;
    server.bandwidth = XDR_P_BANDWIDTH_INVALID;
    server.ant = XDR_P_ANTENNA_DEFAULT;
    server.gain = XDR_P_GAIN_DEFAULT;
    server.daa = XDR_P_DAA_DEFAULT;
    server.squelch = XDR_P_SQUELCH_DEFAULT;
    server.rotator = XDR_P_ROTATOR_DEFAULT;
    server.sampling = XDR_P_SAMPLING_DEFAULT;
    server.detector = XDR_P_DETECTOR_DEFAULT;
}

void tuner_reset(void)
{
    /* restart Arduino using RTS & DTR lines */
    pthread_mutex_lock(&server.mutex_s);
    int ctl;
    if(ioctl(server.serialfd, TIOCMGET, &ctl) != -1)
    {
        ctl &= ~(TIOCM_DTR | TIOCM_RTS);
        ioctl(server.serialfd, TIOCMSET, &ctl);
        usleep(10000);
        ctl |=  (TIOCM_DTR | TIOCM_RTS);
        ioctl(server.serialfd, TIOCMSET, &ctl);
    }
    tuner_defaults();

    /* Wait for controller re-initialization,
       before unlocking the mutex. */
    usleep(XDR_P_ARDUINO_INIT_TIME * 1000);

    pthread_mutex_unlock(&server.mutex_s);
}

void socket_close(int fd)
{
    shutdown(fd, 2);
    close(fd);
}
