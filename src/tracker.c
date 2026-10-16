#include "tracker.h"
#include "net_utils.h"
#include "net_http.h"
#include "bencode.h"
#include "ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/**
 * Log the first bytes of a response with control characters escaped, so the
 * exact line-ending style and body layout are visible in the log.
 */
static void log_response_head(const unsigned char *buf, size_t len)
{
    char text[240];
    size_t i;
    size_t out = 0;

    for (i = 0; i < len && out + 3 < sizeof(text); i++) {
        unsigned char c = buf[i];
        if (c == '\r') {
            text[out++] = '\\';
            text[out++] = 'r';
        } else if (c == '\n') {
            text[out++] = '\\';
            text[out++] = 'n';
        } else if (c >= 0x20 && c < 0x7f) {
            text[out++] = (char)c;
        } else {
            text[out++] = '.';
        }
    }
    text[out] = '\0';

    ui_log("announce: response head: %s", text);
}

/**
 * Parse HTTP response, extract headers and body.
 *
 * Kept deliberately tolerant: trackers in the wild answer with CRLF or with
 * bare LF, and some skip the status line altogether. Dropping those responses
 * means losing every peer they offer.
 */
static int parse_http_response(const unsigned char *resp, size_t resp_len,
                               int *status_code,
                               const unsigned char **body, size_t *body_len)
{
    const char *p = (const char *)resp;
    const char *end = p + resp_len;

    /* Skip leading blank lines and whitespace */
    while (p < end && (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t'))
        p++;

    if (p >= end) return -1;

    /* No HTTP status line: some trackers reply with the bencoded body only */
    if (strncmp(p, "HTTP/", 5) != 0) {
        if (*p == 'd' || *p == 'l') {
            *status_code = 200;
            *body = (const unsigned char *)p;
            *body_len = (size_t)(end - p);
            return 0;
        }
        log_response_head(resp, resp_len);
        return -1;
    }

    /* Find "HTTP/1.x " and extract status code */
    while (p < end && *p != ' ') p++;
    if (p >= end) return -1;
    p++; // skip space

    /* Read status code */
    if (p + 3 > end) return -1;
    *status_code = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');

    /* Find the empty line separating headers from body, accepting both
     * CRLF CRLF and bare LF LF. */
    const char *body_start = NULL;
    for (const char *s = p; s + 1 < end; s++) {
        if (s[0] == '\n' && s[1] == '\n') {
            body_start = s + 2;
            break;
        }
        if (s + 3 < end && s[0] == '\r' && s[1] == '\n' &&
            s[2] == '\r' && s[3] == '\n') {
            body_start = s + 4;
            break;
        }
    }

    if (!body_start) {
        ui_log("announce: HTTP %d but no header separator found in %zu bytes",
               *status_code, resp_len);
        log_response_head(resp, resp_len);
        return -1;
    }

    *body = (const unsigned char *)body_start;
    *body_len = (size_t)(end - body_start);

    return 0;
}

/* ------------------------------------------------------------ trackers UDP */

/**
 * Trackers publicos que se prueban SIEMPRE, ademas de los del .torrent.
 *
 * Motivo medido el 16-10-2026: los trackers del .torrent pueden estar filtrados
 * por la red (el de la pelicula cae a los 5,00 s exactos) o repartir un unico
 * peer (el de Ubuntu). Estos UDP, en cambio, respondieron desde la red del
 * usuario con decenas de peers para el mismo torrent. Para quitar esta lista
 * basta con vaciarla.
 *
 * Solo estan los tres que contestaron de verdad: un tracker UDP muerto cuesta
 * hasta dos timeouts (connect + announce) de panel congelado por ronda.
 */
static const char *extra_trackers[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.demonii.com:1337/announce",
    "udp://tracker.torrent.eu.org:451/announce",
};

const char *const *tracker_extra_list(size_t *count)
{
    if (count) *count = sizeof(extra_trackers) / sizeof(extra_trackers[0]);
    return extra_trackers;
}

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void write_be64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (56 - i * 8));
}

/**
 * Announce to a UDP tracker (BEP-15).
 *
 * Dos peticiones de ida y vuelta: connect (16 bytes) y announce (98 bytes).
 * Timeouts cortos porque esto corre en el mismo hilo que el panel web.
 */
static tracker_response_t *announce_udp(const char *host, uint16_t port,
                                        const tracker_params_t *params)
{
    static const unsigned char protocol_id[8] =
        { 0x00, 0x00, 0x04, 0x17, 0x27, 0x10, 0x19, 0x80 };
    static uint32_t tx_seq = 0x5a17;
    unsigned char req[98];
    unsigned char buf[2048];
    uint32_t addr = 0;
    uint32_t tx = ++tx_seq;
    uint64_t connection_id = 0;
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    int sock;
    int n;

    if (net_resolve(host, &addr) < 0) {
        ui_log("announce: could not resolve '%s'", host);
        return NULL;
    }

    sock = net_udp_open(0);
    if (sock < 0)
        return NULL;

    /* 1. Connect: se pide un connection_id (caduca en un minuto). */
    memset(req, 0, 16);
    memcpy(req, protocol_id, 8);
    write_be32(req + 8, 0);      /* action = connect */
    write_be32(req + 12, tx);

    if (net_udp_send_to(sock, addr, port, req, 16) < 0) {
        net_close(sock);
        return NULL;
    }

    n = net_udp_recv_from_timeout(sock, buf, sizeof(buf), &from_ip, &from_port,
                                  NET_UDP_TRACKER_TIMEOUT_SEC);
    if (n < 16 || read_be32(buf) != 0 || read_be32(buf + 4) != tx) {
        ui_log("announce: UDP %s:%u did not answer the connect", host,
               (unsigned)port);
        net_close(sock);
        return NULL;
    }
    for (int i = 0; i < 8; i++)
        connection_id = (connection_id << 8) | buf[8 + i];

    /* 2. Announce. */
    memset(req, 0, sizeof(req));
    write_be64(req, connection_id);
    write_be32(req + 8, 1);                      /* action = announce */
    write_be32(req + 12, tx);
    memcpy(req + 16, params->info_hash, 20);
    memcpy(req + 36, params->peer_id, 20);
    write_be64(req + 56, (uint64_t)(params->downloaded > 0 ? params->downloaded : 0));
    write_be64(req + 64, (uint64_t)(params->left > 0 ? params->left : 0));
    write_be64(req + 72, (uint64_t)(params->uploaded > 0 ? params->uploaded : 0));
    write_be32(req + 80, 2);                     /* event = started */
    write_be32(req + 84, 0);                     /* ip: que lo deduzca el tracker */
    write_be32(req + 88, tx);                    /* key */
    write_be32(req + 92, MAX_TRACKER_PEERS);     /* num_want */
    req[96] = (unsigned char)(params->port >> 8);
    req[97] = (unsigned char)(params->port & 0xff);

    if (net_udp_send_to(sock, addr, port, req, 98) < 0) {
        net_close(sock);
        return NULL;
    }

    n = net_udp_recv_from_timeout(sock, buf, sizeof(buf), &from_ip, &from_port,
                                  NET_UDP_TRACKER_TIMEOUT_SEC);
    net_close(sock);

    if (n < 20) {
        ui_log("announce: UDP %s:%u did not answer the announce", host,
               (unsigned)port);
        return NULL;
    }

    uint32_t action = read_be32(buf);
    if (action == 3) {
        char reason[128];
        size_t reason_len = (size_t)(n - 8);
        if (reason_len >= sizeof(reason)) reason_len = sizeof(reason) - 1;
        memcpy(reason, buf + 8, reason_len);
        reason[reason_len] = '\0';
        ui_log("announce: UDP %s:%u refused: %s", host, (unsigned)port, reason);
        return NULL;
    }
    if (action != 1) {
        ui_log("announce: UDP %s:%u unexpected action %u", host,
               (unsigned)port, (unsigned)action);
        return NULL;
    }

    tracker_response_t *tr = calloc(1, sizeof(*tr));
    if (!tr)
        return NULL;

    tr->interval = (int)read_be32(buf + 8);
    tr->incomplete = (int)read_be32(buf + 12);
    tr->complete = (int)read_be32(buf + 16);

    int count = (n - 20) / 6;
    if (count > MAX_TRACKER_PEERS) count = MAX_TRACKER_PEERS;
    if (count > 0) {
        tr->peers = calloc((size_t)count, sizeof(peer_addr_t));
        if (tr->peers) {
            for (int i = 0; i < count; i++) {
                memcpy(&tr->peers[i].ip, buf + 20 + (size_t)i * 6, 4);
                memcpy(&tr->peers[i].port, buf + 20 + (size_t)i * 6 + 4, 2);
            }
            tr->num_peers = count;
        }
    }

    ui_log("announce: UDP %s:%u -> %d peer(s), %d seeder(s), %d leecher(s)",
           host, (unsigned)port, tr->num_peers, tr->complete, tr->incomplete);

    return tr;
}

tracker_response_t *tracker_announce(const char *tracker_url,
                                     const tracker_params_t *params)
{
    if (!tracker_url || !params) return NULL;

    /* UDP (BEP-15): la via que funciona en redes que filtran los trackers HTTP.
     * No lleva ruta, solo host:puerto. */
    if (strncmp(tracker_url, "udp://", 6) == 0) {
        const char *rest = tracker_url + 6;
        const char *slash = strchr(rest, '/');
        const char *colon = strchr(rest, ':');
        char udp_host[256];
        int udp_port = 6969;
        size_t udp_host_len;

        if (colon && (!slash || colon < slash)) {
            udp_host_len = (size_t)(colon - rest);
            udp_port = atoi(colon + 1);
        } else {
            udp_host_len = slash ? (size_t)(slash - rest) : strlen(rest);
        }

        if (udp_host_len == 0 || udp_host_len >= sizeof(udp_host))
            return NULL;
        memcpy(udp_host, rest, udp_host_len);
        udp_host[udp_host_len] = '\0';

        if (udp_port <= 0 || udp_port > 65535)
            udp_port = 6969;

        return announce_udp(udp_host, (uint16_t)udp_port, params);
    }

    /* URL del tracker: http://host:port/announce o https://host/announce */
    const char *full_url = tracker_url;
    int use_https = strncmp(full_url, "https://", 8) == 0;
    const char *url;

    if (use_https) {
        /* HTTPS lo sirve la pila de la consola (libSceHttp2 + libSceSsl). */
        if (!net_http_available()) {
            ui_log("announce: %s needs HTTPS and the console HTTP stack is "
                   "not available", full_url);
            return NULL;
        }
        url = full_url + 8;
    } else if (strncmp(full_url, "http://", 7) == 0) {
        url = full_url + 7;
    } else {
        return NULL;
    }

    /* Find host[:port]/path */
    const char *path_start = strchr(url, '/');
    if (!path_start) return NULL;

    /* Parse host and optional port */
    char host[256];
    int port = use_https ? 443 : 80;
    const char *port_str = NULL;
    size_t host_len;

    const char *colon = memchr(url, ':', (size_t)(path_start - url));
    if (colon) {
        host_len = (size_t)(colon - url);
        port_str = colon + 1;
        port = 0;
        while (*port_str >= '0' && *port_str <= '9' && port_str < path_start) {
            port = port * 10 + (*port_str++ - '0');
        }
    } else {
        host_len = (size_t)(path_start - url);
    }

    if (host_len >= sizeof(host)) return NULL;
    memcpy(host, url, host_len);
    host[host_len] = '\0';

    /* Build announce URL parameters */
    char info_hash_enc[100];
    net_url_encode(params->info_hash, 20, info_hash_enc);

    char peer_id_enc[100];
    net_url_encode(params->peer_id, 20, peer_id_enc);

    char query[1024];
    int query_len = snprintf(query, sizeof(query),
        "info_hash=%s"
        "&peer_id=%s"
        "&port=%u"
        "&uploaded=%lld"
        "&downloaded=%lld"
        "&left=%lld"
        "&compact=%d"
        "&numwant=%d",
        info_hash_enc,
        peer_id_enc,
        (unsigned int)params->port,
        (long long)params->uploaded,
        (long long)params->downloaded,
        (long long)params->left,
        params->compact ? 1 : 0,
        MAX_TRACKER_PEERS);

    if (query_len < 0 || (size_t)query_len >= sizeof(query)) return NULL;

    char request[4096];
    int req_len = snprintf(request, sizeof(request),
        "GET %s%c%s"
        " HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: PSTorrent5/1.1\r\n"
        "Connection: close\r\n"
        "\r\n",
        path_start,
        strchr(path_start, '?') ? '&' : '?',
        query,
        host);

    if (req_len < 0 || (size_t)req_len >= sizeof(request)) return NULL;

    /* Transport. HTTPS va por la pila de la consola (que ya devuelve el cuerpo
     * sin cabeceras); HTTP en claro usa el cliente propio. */
    unsigned char resp_buf[HTTP_BUF_SIZE * 4];
    size_t resp_pos = 0;
    int status_code = 0;
    const unsigned char *body = NULL;
    size_t body_len = 0;

    if (use_https) {
        char url_with_query[4096];
        int url_len = snprintf(url_with_query, sizeof(url_with_query), "%s%c%s",
                               full_url, strchr(full_url, '?') ? '&' : '?', query);
        int got;

        if (url_len < 0 || (size_t)url_len >= sizeof(url_with_query)) return NULL;

        got = net_http_get(url_with_query, resp_buf, sizeof(resp_buf),
                           &status_code);
        if (got < 0) {
            ui_log("announce: HTTPS GET %s failed", host);
            return NULL;
        }

        ui_log("announce: HTTPS GET %s -> %d bytes, status %d",
               host, got, status_code);
        resp_pos = (size_t)got;
        body = resp_buf;
        body_len = resp_pos;
    } else {
        /* Resolve host and connect */
        uint32_t addr;

        if (net_resolve(host, &addr) < 0) {
            ui_log("announce: could not resolve '%s'", host);
            return NULL;
        }

        int sock = net_tcp_connect_timeout(addr, (uint16_t)port,
                                           NET_TRACKER_CONNECT_TIMEOUT_SEC);
        if (sock < 0) {
            ui_log("announce: connect to %s:%u failed", host, (unsigned)port);
            return NULL;
        }

        /* Set timeout */
        net_set_timeout(sock, NET_TRACKER_IO_TIMEOUT_SEC);

        /* Send HTTP request */
        if (net_send_all(sock, request, (size_t)req_len) < 0) {
            ui_log("announce: send failed");
            net_close(sock);
            return NULL;
        }

        /* Receive response (use a large buffer) */
        while (resp_pos < sizeof(resp_buf)) {
            int n = net_recv_some(sock, resp_buf + resp_pos,
                                  sizeof(resp_buf) - resp_pos);
            if (n <= 0) break;
            resp_pos += (size_t)n;
        }

        net_close(sock);

        if (resp_pos == 0) return NULL;

        if (parse_http_response(resp_buf, resp_pos, &status_code, &body,
                                &body_len) < 0) {
            ui_log("announce: could not parse the HTTP response (%zu bytes)",
                   resp_pos);
            return NULL;
        }

        ui_log("announce: HTTP %d, body %zu bytes", status_code, body_len);
    }

    tracker_response_t *tr = calloc(1, sizeof(tracker_response_t));
    if (!tr) return NULL;

    if (status_code != 200) {
        /* Try to extract failure reason */
        bcode_node_t *root = bcode_parse(body, body_len);
        if (root) {
            bcode_node_t *fr = bcode_dict_get(root, "failure reason");
            if (fr && fr->type == BCODE_STR) {
                tr->failure_reason = malloc(fr->str_len + 1);
                if (tr->failure_reason) {
                    memcpy(tr->failure_reason, fr->str_val, fr->str_len);
                    tr->failure_reason[fr->str_len] = '\0';
                }
            }
            bcode_free(root);
        }
        if (!tr->failure_reason) {
            tr->failure_reason = strdup("Unknown tracker error");
        }
        return tr;
    }

    /* Parse bencoded tracker response */
    bcode_node_t *root = bcode_parse(body, body_len);
    if (!root) {
        ui_log("announce: tracker response is not bencode");
        tr->failure_reason = strdup("Failed to parse tracker response");
        return tr;
    }

    /* Check for failure */
    bcode_node_t *fr = bcode_dict_get(root, "failure reason");
    if (fr && fr->type == BCODE_STR) {
        tr->failure_reason = malloc(fr->str_len + 1);
        if (tr->failure_reason) {
            memcpy(tr->failure_reason, fr->str_val, fr->str_len);
            tr->failure_reason[fr->str_len] = '\0';
        }
        bcode_free(root);
        return tr;
    }

    /* Parse interval */
    bcode_node_t *interval = bcode_dict_get(root, "interval");
    if (interval) tr->interval = (int)bcode_int_val(interval);
    if (tr->interval < 60) tr->interval = 60; // Minimum 60 seconds

    /* Parse complete/incomplete */
    bcode_node_t *comp = bcode_dict_get(root, "complete");
    if (comp) tr->complete = (int)bcode_int_val(comp);

    bcode_node_t *incomp = bcode_dict_get(root, "incomplete");
    if (incomp) tr->incomplete = (int)bcode_int_val(incomp);

    /* Parse peers */
    bcode_node_t *peers = bcode_dict_get(root, "peers");
    if (peers && peers->type == BCODE_STR) {
        /* Compact response: 6 bytes per peer (4 IP + 2 port) */
        int num_peers = (int)(peers->str_len / 6);
        if (num_peers > MAX_TRACKER_PEERS) num_peers = MAX_TRACKER_PEERS;

        tr->peers = calloc((size_t)num_peers, sizeof(peer_addr_t));
        if (tr->peers) {
            tr->num_peers = num_peers;
            for (int i = 0; i < num_peers; i++) {
                memcpy(&tr->peers[i].ip, peers->str_val + (size_t)i * 6, 4);
                memcpy(&tr->peers[i].port, peers->str_val + (size_t)i * 6 + 4, 2);
            }
        }
    }

    bcode_free(root);
    return tr;
}

void tracker_response_free(tracker_response_t *resp)
{
    if (!resp) return;
    free(resp->peers);
    free(resp->failure_reason);
    free(resp);
}

void tracker_generate_peer_id(unsigned char peer_id[20])
{
    /* Azureus-style peer ID: -PS5T- + random 12 hex digits */
    static const char hex[] = "0123456789abcdef";
    peer_id[0] = '-';
    peer_id[1] = 'P';
    peer_id[2] = 'S';
    peer_id[3] = '5';
    peer_id[4] = 'T';
    peer_id[5] = '-';

    /* Simple pseudo-random - use address or other entropy sources */
    for (int i = 6; i < 20; i++) {
        /* In a real implementation, use a proper PRNG.
         * For the PS5, we use a simple linear congruential generator */
        static unsigned int seed = 0x12345678;
        seed = seed * 1103515245 + 12345;
        peer_id[i] = hex[(seed >> 16) & 0x0F];
    }
}
