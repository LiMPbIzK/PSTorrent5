/*
 * DHT: Kademlia sobre KRPC (BEP-5).
 *
 * Por que existe este fichero: medido el 16-10-2026, torrent.ubuntu.com reparte
 * UN peer por announce (siempre el mismo, 8 announces con peer_id y puerto
 * distintos) y ese peer no soporta PEX. Con 293 seeders en el enjambre y ni
 * tracker util ni PEX posible, la unica via que queda para conseguir peers es
 * preguntar a la red distribuida.
 *
 * Simplificaciones conscientes (ver ARCHITECTURE.md):
 *  - La tabla de nodos es una lista plana: sin k-buckets ni orden por distancia
 *    XOR. Con unos cientos de nodos basta para encontrar peers.
 *  - El ID de nodo es aleatorio (BEP-42 recomienda derivarlo de la IP; no es
 *    necesario para funcionar).
 *  - E/S no bloqueante por select: el motor comparte hilo con el panel web.
 */
#include "dht.h"
#include "net_utils.h"
#include "bencode.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ estado */

typedef struct {
    unsigned char id[20];
    uint32_t      ip;
    uint16_t      port;
    uint64_t      last_seen;
    unsigned char token[32];      // Token para poder anunciarnos en ese nodo
    int           token_len;
    unsigned char query_ih[20];   // info_hash de la ultima consulta enviada
    uint64_t      query_sent;     // cuando se envio (para no dejarla colgada)
    int           query_pending;
    int           used;
} dht_node_t;

typedef struct {
    unsigned char info_hash[20];
    uint64_t      added;
    int           used;
} dht_want_t;

static int             sock = -1;
static unsigned char   my_id[20];
static dht_peer_cb_t   on_peer;
static dht_node_t      nodes[DHT_MAX_NODES];
static dht_want_t      wants[DHT_MAX_WANTS];
static int             node_count;
static int             peers_found;
static uint64_t        last_bootstrap;
static uint64_t        last_get_peers;
static uint64_t        last_announce;
static int             query_cursor;

static const struct { const char *host; uint16_t port; } routers[] = {
    { "router.bittorrent.com",  6881 },
    { "router.utorrent.com",    6881 },
    { "dht.transmissionbt.com", 6881 },
    { "dht.libtorrent.org",    25401 },
};
#define ROUTER_COUNT ((int)(sizeof(routers) / sizeof(routers[0])))
static uint32_t router_ip[ROUTER_COUNT];

static uint64_t now_sec(void)
{
    return (uint64_t)time(NULL);
}

static void random_id(unsigned char out[20])
{
    FILE *f = fopen("/dev/urandom", "rb");

    if (f) {
        size_t got = fread(out, 1, 20, f);
        fclose(f);
        if (got == 20)
            return;
    }

    /* Reserva: no es criptografico, pero para un ID de nodo sirve. */
    srand((unsigned)(time(NULL) ^ (long)getpid()));
    for (int i = 0; i < 20; i++)
        out[i] = (unsigned char)(rand() & 0xff);
}

/* ------------------------------------------------ escritor de bencode minimo
 *
 * Las consultas KRPC son pequenas y de forma fija. Se construyen a mano (con
 * las longitudes calculadas por snprintf) porque el modulo de bencode del
 * proyecto solo parsea, no construye. */
typedef struct {
    unsigned char *buf;
    size_t         cap;
    size_t         pos;
    int            overflow;
} bw_t;

static void bw_raw(bw_t *w, const void *p, size_t n)
{
    if (w->overflow) return;
    if (w->pos + n > w->cap) { w->overflow = 1; return; }
    if (n > 0) memcpy(w->buf + w->pos, p, n);
    w->pos += n;
}

static void bw_byte(bw_t *w, char c)
{
    bw_raw(w, &c, 1);
}

static void bw_str(bw_t *w, const void *p, size_t n)
{
    char head[24];
    int h = snprintf(head, sizeof(head), "%zu:", n);

    if (h < 0 || (size_t)h >= sizeof(head)) { w->overflow = 1; return; }
    bw_raw(w, head, (size_t)h);
    bw_raw(w, p, n);
}

static void bw_key(bw_t *w, const char *key)
{
    bw_str(w, key, strlen(key));
}

static void bw_text(bw_t *w, const char *text)
{
    bw_str(w, text, strlen(text));
}

/* ------------------------------------------------------------- consultas */

static size_t build_find_node_payload(unsigned char *buf, size_t cap,
                                      const unsigned char target[20])
{
    bw_t w = { buf, cap, 0, 0 };

    bw_key(&w, "id");
    bw_str(&w, my_id, 20);
    bw_key(&w, "target");
    bw_str(&w, target, 20);

    return w.overflow ? 0 : w.pos;
}

static size_t build_get_peers_payload(unsigned char *buf, size_t cap,
                                      const unsigned char info_hash[20])
{
    bw_t w = { buf, cap, 0, 0 };

    bw_key(&w, "id");
    bw_str(&w, my_id, 20);
    bw_key(&w, "info_hash");
    bw_str(&w, info_hash, 20);

    return w.overflow ? 0 : w.pos;
}

static size_t build_announce_payload(unsigned char *buf, size_t cap,
                                     const unsigned char info_hash[20],
                                     const unsigned char *token, size_t token_len)
{
    bw_t w = { buf, cap, 0, 0 };
    char port[24];
    int port_len = snprintf(port, sizeof(port), "i%ue", (unsigned)6881);

    if (port_len < 0) return 0;

    /* Claves ordenadas como manda el bencode: id < implied_port < info_hash <
     * port < token. */
    bw_key(&w, "id");
    bw_str(&w, my_id, 20);
    bw_key(&w, "implied_port");
    bw_raw(&w, "i0e", 3);
    bw_key(&w, "info_hash");
    bw_str(&w, info_hash, 20);
    bw_key(&w, "port");
    bw_raw(&w, port, (size_t)port_len);
    bw_key(&w, "token");
    bw_str(&w, token, token_len);

    return w.overflow ? 0 : w.pos;
}

/** Envuelve un payload ya construido en una consulta KRPC. */
static size_t build_query(unsigned char *buf, size_t cap, const char *method,
                          const unsigned char *payload, size_t payload_len)
{
    static unsigned char tid = 0;
    unsigned char t[2];
    bw_t w = { buf, cap, 0, 0 };

    if (payload_len == 0)
        return 0;

    t[0] = (unsigned char)('a' + (tid % 26));
    t[1] = (unsigned char)('0' + (tid / 26) % 10);
    tid++;

    bw_byte(&w, 'd');
    bw_key(&w, "a");
    bw_byte(&w, 'd');
    bw_raw(&w, payload, payload_len);
    bw_byte(&w, 'e');
    bw_key(&w, "q");
    bw_text(&w, method);
    bw_key(&w, "t");
    bw_str(&w, t, 2);
    bw_key(&w, "y");
    bw_text(&w, "q");
    bw_byte(&w, 'e');

    return w.overflow ? 0 : w.pos;
}

static int send_query(uint32_t addr, uint16_t port, const char *method,
                      const unsigned char *payload, size_t payload_len)
{
    unsigned char buf[512];
    size_t len;

    if (sock < 0)
        return -1;

    len = build_query(buf, sizeof(buf), method, payload, payload_len);
    if (len == 0)
        return -1;

    return net_udp_send_to(sock, addr, port, buf, len);
}

/* ----------------------------------------------------------- tabla de nodos */

static void node_add(const unsigned char id[20], uint32_t ip, uint16_t port,
                     uint64_t now)
{
    int free_slot = -1;
    int oldest = 0;

    if (ip == 0 || port == 0)
        return;

    for (int i = 0; i < DHT_MAX_NODES; i++) {
        if (!nodes[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (nodes[i].ip == ip && nodes[i].port == port) {
            memcpy(nodes[i].id, id, 20);
            nodes[i].last_seen = now;
            return;
        }
        if (nodes[i].last_seen < nodes[oldest].last_seen)
            oldest = i;
    }

    if (free_slot < 0) {
        /* Tabla llena: se tira el mas viejo (no hay bucles de refresco). */
        free_slot = oldest;
        if (nodes[free_slot].used)
            node_count--;
    }

    memcpy(nodes[free_slot].id, id, 20);
    nodes[free_slot].ip = ip;
    nodes[free_slot].port = port;
    nodes[free_slot].last_seen = now;
    nodes[free_slot].token_len = 0;
    nodes[free_slot].query_pending = 0;
    nodes[free_slot].used = 1;
    node_count++;
}

static void node_forget(int idx)
{
    if (!nodes[idx].used) return;
    nodes[idx].used = 0;
    if (node_count > 0) node_count--;
}

/* ------------------------------------------------------------ wants */

void dht_want(const unsigned char info_hash[20])
{
    uint64_t now = now_sec();
    int free_slot = -1;
    int oldest = 0;

    for (int i = 0; i < DHT_MAX_WANTS; i++) {
        if (wants[i].used) {
            if (memcmp(wants[i].info_hash, info_hash, 20) == 0)
                return;
            if (wants[i].added < wants[oldest].added)
                oldest = i;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0)
        free_slot = oldest;

    memcpy(wants[free_slot].info_hash, info_hash, 20);
    wants[free_slot].added = now;
    wants[free_slot].used = 1;
}

void dht_unwant(const unsigned char info_hash[20])
{
    for (int i = 0; i < DHT_MAX_WANTS; i++) {
        if (wants[i].used && memcmp(wants[i].info_hash, info_hash, 20) == 0)
            wants[i].used = 0;
    }
}

/* ------------------------------------------------------- tramas entrantes */

/** Guarda los nodos que vienen en un campo compacto (26 bytes cada uno). */
static void handle_compact_nodes(const unsigned char *buf, size_t len,
                                 uint64_t now)
{
    for (size_t off = 0; off + 26 <= len; off += 26) {
        uint32_t ip;
        uint16_t port;

        memcpy(&ip, buf + off + 20, 4);
        memcpy(&port, buf + off + 24, 2);
        node_add(buf + off, ip, ntohs(port), now);
    }
}

/** Entrega un peer suelto (6 bytes: ip + puerto) al torrent correspondiente. */
static void deliver_peer(const unsigned char *raw,
                         const unsigned char info_hash[20])
{
    peer_addr_t peer;

    memset(&peer, 0, sizeof(peer));
    memcpy(&peer.ip, raw, 4);
    memcpy(&peer.port, raw + 4, 2);
    if (peer.port == 0)
        return;

    peers_found++;
    if (on_peer)
        on_peer(info_hash, &peer);
}

/** Avisa de los peers de un campo "values". */
static void handle_values(const bcode_node_t *values,
                          const unsigned char info_hash[20])
{
    if (!values)
        return;

    /* Lo normal es una lista de cadenas de 6 bytes, pero hay nodos que mandan
     * el unico peer como una cadena suelta. */
    if (values->type == BCODE_STR) {
        size_t len = 0;
        const unsigned char *raw = bcode_str_val(values, &len);

        if (raw && len >= 6)
            deliver_peer(raw, info_hash);
        return;
    }

    size_t count = bcode_list_len(values);

    for (size_t i = 0; i < count; i++) {
        bcode_node_t *item = bcode_list_get(values, i);
        size_t len = 0;
        const unsigned char *raw = item ? bcode_str_val(item, &len) : NULL;

        if (raw && len >= 6)
            deliver_peer(raw, info_hash);
    }
}

static void handle_packet(const unsigned char *buf, size_t len,
                          uint32_t from_ip, uint16_t from_port, uint64_t now)
{
    bcode_node_t *root = bcode_parse(buf, len);
    bcode_node_t *y;
    size_t y_len = 0;
    const unsigned char *y_val;

    if (!root)
        return;

    y = bcode_dict_get(root, "y");
    y_val = y ? bcode_str_val(y, &y_len) : NULL;
    if (!y_val || y_len != 1) {
        bcode_free(root);
        return;
    }

    if (y_val[0] == 'r') {
        bcode_node_t *r = bcode_dict_get(root, "r");

        if (!r) { bcode_free(root); return; }

        bcode_node_t *id = bcode_dict_get(r, "id");
        size_t id_len = 0;
        const unsigned char *id_val = id ? bcode_str_val(id, &id_len) : NULL;

        /* Localizar el nodo en la tabla: refrescar su marca de vida si ya lo
         * conociamos, o darlo de alta con el id que acaba de decirnos. */
        int idx = -1;
        for (int i = 0; i < DHT_MAX_NODES; i++) {
            if (nodes[i].used && nodes[i].ip == from_ip &&
                nodes[i].port == from_port) {
                idx = i;
                break;
            }
        }

        if (idx >= 0) {
            nodes[idx].last_seen = now;
            if (id_val && id_len == 20)
                memcpy(nodes[idx].id, id_val, 20);
        } else if (id_val && id_len == 20) {
            node_add(id_val, from_ip, from_port, now);
            for (int i = 0; i < DHT_MAX_NODES; i++) {
                if (nodes[i].used && nodes[i].ip == from_ip &&
                    nodes[i].port == from_port) {
                    idx = i;
                    break;
                }
            }
        }

        bcode_node_t *nodes_field = bcode_dict_get(r, "nodes");
        size_t nodes_len = 0;
        const unsigned char *nodes_raw = nodes_field ? bcode_str_val(nodes_field, &nodes_len) : NULL;
        if (nodes_raw)
            handle_compact_nodes(nodes_raw, nodes_len, now);

        if (idx >= 0) {
            bcode_node_t *token = bcode_dict_get(r, "token");
            size_t token_len = 0;
            const unsigned char *token_raw = token ? bcode_str_val(token, &token_len) : NULL;

            if (token_raw && token_len > 0 && token_len <= sizeof(nodes[idx].token)) {
                memcpy(nodes[idx].token, token_raw, token_len);
                nodes[idx].token_len = (int)token_len;
            }
        }

        bcode_node_t *values = bcode_dict_get(r, "values");
        if (idx >= 0 && nodes[idx].query_pending)
            handle_values(values, nodes[idx].query_ih);

        if (idx >= 0)
            nodes[idx].query_pending = 0;
    } else if (y_val[0] == 'e') {
        bcode_node_t *err = bcode_dict_get(root, "e");
        bcode_node_t *code = err ? bcode_list_get(err, 0) : NULL;

        ui_log("dht: %u.%u.%u.%u:%u answered error %lld",
               (ntohl(from_ip) >> 24) & 0xff, (ntohl(from_ip) >> 16) & 0xff,
               (ntohl(from_ip) >> 8) & 0xff, ntohl(from_ip) & 0xff,
               (unsigned)from_port,
               (long long)(code ? bcode_int_val(code) : 0));
    }

    bcode_free(root);
}

/* --------------------------------------------------------------- bootstrap */

static void dht_bootstrap(int verbose)
{
    unsigned char payload[128];
    size_t len = build_find_node_payload(payload, sizeof(payload), my_id);
    int resolved = 0;

    for (int i = 0; i < ROUTER_COUNT; i++) {
        if (router_ip[i] == 0) {
            if (net_resolve(routers[i].host, &router_ip[i]) < 0) {
                if (verbose)
                    ui_log("dht: could not resolve %s", routers[i].host);
                continue;
            }
        }
        resolved++;
        if (send_query(router_ip[i], routers[i].port, "find_node", payload, len) < 0 &&
            verbose)
            ui_log("dht: could not query %s", routers[i].host);
    }

    if (!verbose)
        return;

    if (resolved > 0)
        ui_log("dht: bootstrapping against %d router(s)", resolved);

    /* Si tras varias vueltas sigue sin haber ni un nodo, lo mas probable es que
     * la red filtre los routers DHT. Medido el 16-10-2026: en esta red
     * responden NTP, DNS y los trackers UDP, pero router.bittorrent.com no
     * contesta. Se deja constancia cada ~2 minutos, no en cada intento. */
    if (node_count == 0)
        ui_log("dht: still no nodes; the DHT routers may be filtered here");
}

/* --------------------------------------------------------------------- tick */

void dht_tick(void)
{
    uint64_t now = now_sec();

    if (sock < 0)
        return;

    /* 1. Vaciar el socket (select con tiempo cero: nunca bloquea). */
    for (int guard = 0; guard < 32; guard++) {
        unsigned char buf[DHT_MAX_DATAGRAM];
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int n = net_udp_recv_from(sock, buf, sizeof(buf), &from_ip, &from_port);

        if (n <= 0)
            break;
        handle_packet(buf, (size_t)n, from_ip, from_port, now);
    }

    /* 2. Bootstrap: al arrancar y luego solo si la tabla se queda corta. Solo se
     *    registra de vez en cuando, o el log se llenaria de reintentos. */
    if (now - last_bootstrap >= (node_count < 8 ? 15 : 300)) {
        static int attempts = 0;
        int first = (last_bootstrap == 0);

        last_bootstrap = now;
        attempts++;
        dht_bootstrap(first || (attempts % 8 == 0));
    }

    /* 3. Preguntar por cada info_hash que nos interesa, repartiendo entre los
     *    nodos conocidos para no dar siempre al mismo. */
    if (now - last_get_peers >= 5) {
        last_get_peers = now;

        for (int w = 0; w < DHT_MAX_WANTS; w++) {
            unsigned char payload[128];
            int sent = 0;

            if (!wants[w].used)
                continue;

            size_t len = build_get_peers_payload(payload, sizeof(payload),
                                                 wants[w].info_hash);
            if (len == 0)
                continue;

            for (int k = 0; k < DHT_MAX_NODES && sent < 6; k++) {
                int idx = (query_cursor + k) % DHT_MAX_NODES;

                if (!nodes[idx].used || nodes[idx].query_pending)
                    continue;

                if (send_query(nodes[idx].ip, nodes[idx].port, "get_peers",
                               payload, len) < 0)
                    continue;

                memcpy(nodes[idx].query_ih, wants[w].info_hash, 20);
                nodes[idx].query_pending = 1;
                nodes[idx].query_sent = now;
                sent++;
            }

            if (sent > 0)
                query_cursor = (query_cursor + sent) % DHT_MAX_NODES;
        }
    }

    /* 4. Anunciarnos: asi otros nos encuentran a nosotros. Se hace con el token
     *    que nos dio cada nodo, uno por vuelta. */
    if (now - last_announce >= 20) {
        last_announce = now;

        for (int i = 0; i < DHT_MAX_NODES; i++) {
            unsigned char payload[160];
            size_t len;

            if (!nodes[i].used || nodes[i].token_len == 0)
                continue;

            len = build_announce_payload(payload, sizeof(payload),
                                         nodes[i].query_ih, nodes[i].token,
                                         (size_t)nodes[i].token_len);
            if (len > 0)
                send_query(nodes[i].ip, nodes[i].port, "announce_peer",
                           payload, len);

            /* El token es de un solo uso para nosotros: se consume. */
            nodes[i].token_len = 0;
            break;
        }
    }

    /* 5. Olvidar nodos que llevan mucho callados y desbloquear consultas que se
     *    quedaron sin respuesta (si no, ese nodo no se volveria a usar nunca). */
    for (int i = 0; i < DHT_MAX_NODES; i++) {
        if (!nodes[i].used)
            continue;
        if (now - nodes[i].last_seen > 900)
            node_forget(i);
        else if (nodes[i].query_pending && now - nodes[i].query_sent > 20)
            nodes[i].query_pending = 0;
    }
}

/* ------------------------------------------------------------------- varios */

int dht_init(dht_peer_cb_t peer_cb)
{
    if (!peer_cb)
        return -1;

    on_peer = peer_cb;
    random_id(my_id);

    sock = net_udp_open(0);
    if (sock < 0) {
        ui_log("dht: could not open the UDP socket");
        return -1;
    }

    ui_log("dht: node %02x%02x%02x%02x... listening on an ephemeral port",
           my_id[0], my_id[1], my_id[2], my_id[3]);

    /* Autoprueba del formato (bencode y KRPC van con longitudes por delante: un
     * digito de mas y todos los nodos ignoran la consulta en silencio). Se
     * valida la propia consulta con el parser del proyecto. */
    {
        unsigned char payload[160];
        unsigned char query[512];
        size_t plen = build_find_node_payload(payload, sizeof(payload), my_id);
        size_t qlen = plen ? build_query(query, sizeof(query), "find_node",
                                         payload, plen) : 0;
        bcode_node_t *root = qlen ? bcode_parse(query, qlen) : NULL;
        bcode_node_t *q = root ? bcode_dict_get(root, "q") : NULL;

        if (!q)
            ui_log("BUG: DHT queries are malformed; the DHT will not work");
        else
            ui_log("dht: query format ok");
        bcode_free(root);
    }

    /* Bootstrap ya, sin esperar al primer tick. */
    dht_bootstrap(1);

    return 0;
}

void dht_shutdown(void)
{
    if (sock >= 0) {
        net_close(sock);
        sock = -1;
    }
    memset(nodes, 0, sizeof(nodes));
    memset(wants, 0, sizeof(wants));
    node_count = 0;
}

int dht_node_count(void)    { return node_count; }
int dht_peers_found(void)   { return peers_found; }

void dht_log_status(void)
{
    ui_log("dht: %d node(s), %d peer(s) found", node_count, peers_found);
}
