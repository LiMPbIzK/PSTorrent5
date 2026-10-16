#include "net_utils.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

int net_init(void)
{
    /* On FreeBSD (PS5), networking is already initialized.
     * Just return success. */
    return 0;
}

void net_cleanup(void)
{
    /* Nothing to clean up on FreeBSD */
}

int net_resolve(const char *hostname, uint32_t *addr_out)
{
    if (!hostname || !addr_out) return -1;

    /* Try as IP address first */
    struct in_addr in;
    if (inet_pton(AF_INET, hostname, &in) == 1) {
        *addr_out = in.s_addr;
        return 0;
    }

    /* Un nombre de una sola etiqueta (sin punto) no es un tracker legitimo y
     * dispara una busqueda DNS que puede bloquear el hilo principal: el motor
     * de torrents comparte hilo con el panel web, asi que se rechaza antes de
     * llegar al resolver. */
    if (!strchr(hostname, '.') && strcmp(hostname, "localhost") != 0)
        return -1;

    /* Try DNS resolution */
    struct hostent *he = gethostbyname(hostname);
    if (!he || he->h_addrtype != AF_INET) {
        ui_log("dns: '%s' failed (h_errno=%d: %s)", hostname, h_errno,
               hstrerror(h_errno));
        return -1;
    }

    memcpy(addr_out, he->h_addr_list[0], 4);
    return 0;
}

int net_tcp_connect_timeout(uint32_t addr, uint16_t port, int timeout_sec)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = addr;
    saddr.sin_port = htons(port);

    /* Non-blocking connect: this code runs on the same thread as the web
     * server, so a peer that silently drops SYNs must not stall the panel. */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        if (errno != EINPROGRESS) {
            close(sock);
            return -1;
        }

        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);

        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        int ready = select(sock + 1, NULL, &writefds, NULL, &tv);
        if (ready <= 0) {
            close(sock);
            return -1;
        }

        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0 ||
            so_error != 0) {
            close(sock);
            return -1;
        }
    }

    /* Restore blocking mode for the plain send/recv helpers. */
    if (fcntl(sock, F_SETFL, flags) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

int net_tcp_connect(uint32_t addr, uint16_t port)
{
    return net_tcp_connect_timeout(addr, port, NET_TRACKER_CONNECT_TIMEOUT_SEC);
}

int net_send_all(int sock, const void *data, size_t len)
{
    const unsigned char *ptr = (const unsigned char *)data;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(sock, ptr, remaining, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent == 0) return -1;
        ptr += sent;
        remaining -= (size_t)sent;
    }

    return 0;
}

int net_recv_exact(int sock, void *buf, size_t len)
{
    unsigned char *ptr = (unsigned char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = recv(sock, ptr, remaining, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            /* Cierre limpio del peer. Se deja errno a 0 para que quien registre
             * el error pueda distinguirlo de un reset (104) o de un timeout. */
            errno = 0;
            return -1;
        }
        ptr += n;
        remaining -= (size_t)n;
    }

    return 0;
}

int net_recv_some(int sock, void *buf, size_t len)
{
    ssize_t n = recv(sock, buf, len, 0);
    if (n < 0) {
        if (errno == EINTR) return net_recv_some(sock, buf, len);
        return -1;
    }
    return (int)n;
}

int net_set_timeout(int sock, int timeout_sec)
{
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return -1;

    return 0;
}

void net_close(int sock)
{
    if (sock >= 0) close(sock);
}

int net_udp_open(uint16_t local_port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) return -1;

    /* Para hablar DHT hay que poder recibir respuestas en el mismo puerto desde
     * el que se pregunta, asi que se ata a un puerto (0 = el que de el sistema). */
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(local_port);

    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ui_log("udp: bind to port %u failed", (unsigned)local_port);
        close(sock);
        return -1;
    }

    return sock;
}

int net_udp_send_to(int sock, uint32_t addr, uint16_t port,
                    const void *data, size_t len)
{
    struct sockaddr_in dst;

    if (sock < 0) return -1;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = addr;
    dst.sin_port = htons(port);

    if (sendto(sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        if (errno == EINTR)
            return net_udp_send_to(sock, addr, port, data, len);
        return -1;
    }

    return 0;
}

int net_udp_recv_from_timeout(int sock, void *buf, size_t len,
                              uint32_t *from_addr, uint16_t *from_port,
                              int timeout_sec)
{
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    ssize_t n;
    fd_set readfds;
    struct timeval tv;
    int ready;

    if (sock < 0) return -1;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    ready = select(sock + 1, &readfds, NULL, NULL, &tv);
    if (ready == 0) return 0;   /* no habia nada: no es un error */
    if (ready < 0) return -1;

    n = recvfrom(sock, buf, len, 0, (struct sockaddr *)&src, &src_len);
    if (n < 0)
        return -1;

    if (from_addr) *from_addr = src.sin_addr.s_addr;
    if (from_port) *from_port = ntohs(src.sin_port);

    return (int)n;
}

int net_udp_recv_from(int sock, void *buf, size_t len,
                      uint32_t *from_addr, uint16_t *from_port)
{
    return net_udp_recv_from_timeout(sock, buf, len, from_addr, from_port, 0);
}

void net_url_encode(const unsigned char *input, size_t input_len, char *output)
{
    static const char hex[] = "0123456789abcdef";
    size_t pos = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = input[i];
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            c == '.' || c == '-' || c == '_' || c == '~') {
            output[pos++] = (char)c;
        } else {
            output[pos++] = '%';
            output[pos++] = hex[c >> 4];
            output[pos++] = hex[c & 0x0F];
        }
    }

    output[pos] = '\0';
}
