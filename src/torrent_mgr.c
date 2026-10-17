#include "torrent_mgr.h"
#include "net_utils.h"
#include "sha1.h"
#include "http_server.h"
#include "bencode.h"
#include "version.h"
#include "dht.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

/**
 * Managed torrents array.
 */
static managed_torrent_t torrents[MAX_TORRENTS];
static int num_torrents = 0;

static int ensure_directory_tree(const char *path)
{
    char current[512];
    size_t len;

    if (!path || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    len = strlen(path);
    if (len == 0 || len >= sizeof(current)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(current, path, len + 1);
    while (len > 1 && current[len - 1] == '/')
        current[--len] = '\0';

    for (char *cursor = current + 1;; cursor++) {
        if (*cursor != '/' && *cursor != '\0') continue;

        char separator = *cursor;
        struct stat st;
        *cursor = '\0';

        if (mkdir(current, 0755) < 0 && errno != EEXIST) {
            *cursor = separator;
            return -1;
        }
        if (stat(current, &st) < 0) {
            *cursor = separator;
            return -1;
        }
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            *cursor = separator;
            return -1;
        }

        *cursor = separator;
        if (separator == '\0') break;
    }

    return 0;
}

/**
 * Get current time in seconds (monotonic-ish).
 */
static uint64_t get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

void torrent_mgr_init(void)
{
    memset(torrents, 0, sizeof(torrents));
    num_torrents = 0;

    /* Autoprueba del handshake extendido (BEP-10). Un bencode mal formado no da
     * error en ninguna parte: el peer simplemente cierra la conexion al recibirlo
     * y desde fuera parece que "el peer se cae solo". Paso una vez, por un
     * digito de mas en la longitud de una cadena. */
    {
        char dict[96];
        static const char version[] = "PSTorrent5";
        static const char pex_key[] = "ut_pex";
        int len = snprintf(dict, sizeof(dict), "d1:md%zu:%si%dee1:v%zu:%se",
                           sizeof(pex_key) - 1, pex_key, PEER_EXT_UT_PEX,
                           sizeof(version) - 1, version);
        bcode_node_t *root = (len > 0) ? bcode_parse((const unsigned char *)dict,
                                                     (size_t)len) : NULL;
        bcode_node_t *m = root ? bcode_dict_get(root, "m") : NULL;
        bcode_node_t *pex = m ? bcode_dict_get(m, "ut_pex") : NULL;

        if (!pex)
            ui_log("BUG: extended handshake malformed; PEX will not work");
        else
            ui_log("PEX ready: ut_pex id %d", (int)bcode_int_val(pex));
        bcode_free(root);
    }
}

int torrent_mgr_add(torrent_t *torrent, const char *save_path,
                    int is_magnet, const char *magnet_uri)
{
    if (!torrent) return -1;
    if (num_torrents >= MAX_TORRENTS) return -1;

    int idx = num_torrents;
    managed_torrent_t *mt = &torrents[idx];
    memset(mt, 0, sizeof(managed_torrent_t));

    /* Set ID from info hash */
    char hash_hex[41];
    torrent_info_hash_str(torrent, hash_hex);
    strncpy(mt->id, hash_hex, sizeof(mt->id) - 1);

    /* Set name */
    if (torrent->name)
        strncpy(mt->name, torrent->name, sizeof(mt->name) - 1);
    else
        snprintf(mt->name, sizeof(mt->name), "Torrent_%s", hash_hex);

    strncpy(mt->save_path, save_path, sizeof(mt->save_path) - 1);
    memcpy(mt->info_hash, torrent->info_hash, 20);
    mt->torrent = torrent;
    mt->file_writer.fd = -1;
    mt->total_size = torrent->total_size;
    mt->is_magnet = is_magnet;
    if (magnet_uri)
        strncpy(mt->magnet_uri, magnet_uri, sizeof(mt->magnet_uri) - 1);

    /* Initialize peer connections */
    for (int i = 0; i < MAX_PEERS_PER_TORRENT; i++)
        mt->peers[i].sock = -1;

    /* Generate peer ID */
    tracker_generate_peer_id(mt->peer_id);

    mt->state = TORRENT_STOPPED;
    num_torrents++;

    return idx;
}

int torrent_mgr_add_raw(const unsigned char *data, size_t data_len,
                        const char *save_path)
{
    torrent_t *t = torrent_parse(data, data_len);
    if (!t) return -1;
    return torrent_mgr_add(t, save_path, 0, NULL);
}

int torrent_mgr_add_magnet(const char *magnet_uri, const char *save_path)
{
    /* Parse magnet URI: magnet:?xt=urn:btih:<infohash>&dn=<name>&tr=<tracker> */
    if (!magnet_uri || strncmp(magnet_uri, "magnet:", 7) != 0)
        return -1;

    /* Extract info hash */
    const char *xt = strstr(magnet_uri, "xt=urn:btih:");
    if (!xt) return -1;
    xt += 12; // skip "xt=urn:btih:"

    char hash_str[41];
    int hash_pos = 0;
    while (*xt && *xt != '&' && *xt != '?' && hash_pos < 40) {
        hash_str[hash_pos++] = *xt++;
    }
    hash_str[hash_pos] = '\0';

    /* Convert hex hash to binary */
    unsigned char info_hash[20];
    for (int i = 0; i < 20; i++) {
        char byte[3] = {hash_str[i*2], hash_str[i*2+1], 0};
        info_hash[i] = (unsigned char)strtol(byte, NULL, 16);
    }

    /* Extract display name */
    char display_name[256] = {0};
    const char *dn = strstr(magnet_uri, "dn=");
    if (dn) {
        dn += 3;
        int dn_pos = 0;
        while (*dn && *dn != '&' && dn_pos < 255) {
            display_name[dn_pos++] = *dn++;
        }
        display_name[dn_pos] = '\0';
        http_url_decode(display_name);
    }

    /* Build a minimal torrent_t from the magnet data */
    torrent_t *t = calloc(1, sizeof(torrent_t));
    if (!t) return -1;

    memcpy(t->info_hash, info_hash, 20);

    if (display_name[0]) {
        t->name = strdup(display_name);
        if (t->name) t->name_len = strlen(t->name);
    } else {
        t->name = strdup(hash_str);
        if (t->name) t->name_len = 40;
    }

    /* Extract tracker URLs */
    const char *tr_ptr = magnet_uri;
    int tracker_count = 0;
    t->announce_list = calloc(8, sizeof(char *));

    while ((tr_ptr = strstr(tr_ptr, "tr=")) != NULL && tracker_count < 8) {
        tr_ptr += 3;
        const char *tr_end = strchr(tr_ptr, '&');
        size_t tr_len = tr_end ? (size_t)(tr_end - tr_ptr) : strlen(tr_ptr);

        char *decoded = malloc(tr_len + 1);
        if (decoded) {
            memcpy(decoded, tr_ptr, tr_len);
            decoded[tr_len] = '\0';
            http_url_decode(decoded);

            t->announce_list[tracker_count++] = decoded;

            /* First tracker is the primary announce */
            if (tracker_count == 1) {
                t->announce = strdup(decoded);
                if (t->announce) t->announce_len = strlen(decoded);
            }
        }

        if (!tr_end) break;
        tr_ptr = tr_end;
    }
    t->announce_list_count = (size_t)tracker_count;

    /* Magnet links don't have piece info - we need to get metadata
     * from peers first. For now, mark as magnet and return.
     * The download will start when we connect to peers and get metadata. */
    t->piece_length = 0;
    t->num_pieces = 0;
    t->total_size = 0;
    t->is_multi_file = 0;

    int idx = torrent_mgr_add(t, save_path, 1, magnet_uri);

    /* If torrent_mgr_add failed, free the torrent we allocated */
    if (idx < 0) {
        torrent_free(t);
        return -1;
    }

    return idx;
}

/**
 * Descarta la pieza en curso. La pieza sigue marcada como libre en el gestor,
 * asi que volvera a salir en la siguiente seleccion: es descarga redundante
 * pero nunca deja una pieza a medias.
 */
static void rx_reset(managed_torrent_t *mt)
{
    mt->rx_piece = -1;
    mt->rx_length = 0;
    mt->rx_have = 0;
    mt->rx_next = 0;
    mt->rx_pending = 0;
    mt->rx_peer = -1;
}

/**
 * Libera el buffer de pieza y reinicia el estado de descarga.
 */
static void rx_teardown(managed_torrent_t *mt)
{
    free(mt->rx_buf);
    mt->rx_buf = NULL;
    rx_reset(mt);
}

/**
 * Abandona la pieza en curso devolviendola a la cola del gestor (si no, se
 * quedaria marcada como pedida y no se volveria a repartir nunca).
 */
static void rx_drop(managed_torrent_t *mt)
{
    if (mt->rx_piece >= 0)
        piece_mgr_failed(&mt->piece_mgr, (size_t)mt->rx_piece);
    rx_reset(mt);
}

/**
 * Cierra una conexion de peer y deja el hueco reutilizable.
 *
 * Se centraliza aqui porque se cierra desde tres sitios (error de lectura,
 * fallo de keep-alive y parada del torrent) y olvidar el `rx_drop` deja la
 * pieza a medias marcada como pedida para siempre.
 */
static void peer_close(managed_torrent_t *mt, int peer_idx)
{
    torrent_peer_t *p = &mt->peers[peer_idx];

    if (p->sock >= 0)
        net_close(p->sock);
    p->sock = -1;
    free(p->bitfield);
    p->bitfield = NULL;
    p->bitfield_len = 0;
    p->has_bitfield = 0;

    if (mt->active_peers > 0)
        mt->active_peers--;
    if (mt->rx_peer == peer_idx)
        rx_drop(mt);
}

int torrent_mgr_start(int index)
{
    if (index < 0 || index >= num_torrents) return -1;
    managed_torrent_t *mt = &torrents[index];

    if (mt->state != TORRENT_STOPPED && mt->state != TORRENT_ERROR)
        return -1;
    if (!mt->torrent) return -1;

    if (mt->state == TORRENT_ERROR) {
        piece_mgr_destroy(&mt->piece_mgr);
        file_writer_destroy(&mt->file_writer);
    }
    mt->error_type = TERR_NONE;
    mt->error_msg[0] = '\0';
    mt->error_code[0] = '\0';

    /* Marca de build: con esto se sabe que ELF esta corriendo de verdad sin
     * tener que adivinarlo (el log del payload se corta y se reanuda). */
    ui_log("engine: PSTorrent5 %s (build " __DATE__ " " __TIME__ ")",
           PSTORRENT5_VERSION);

    ui_log("start #%d '%s': %zu pieces x %lld B, %zu files, dest=%s",
           index, mt->name, mt->torrent->num_pieces,
           (long long)mt->torrent->piece_length, mt->torrent->num_files,
           mt->save_path);

    /* Interesarse por este info_hash en la DHT: es la fuente de peers que no
     * depende del tracker. */
    dht_want(mt->info_hash);

    /* Create save directory */
    if (ensure_directory_tree(mt->save_path) < 0) {
        int saved_errno = errno;
        mt->state = TORRENT_ERROR;
        mt->error_type = TERR_WRITE_FAILED;
        strncpy(mt->error_code, "write_failed", sizeof(mt->error_code) - 1);
        if (saved_errno == EACCES || saved_errno == EPERM) {
            snprintf(mt->error_msg, sizeof(mt->error_msg),
                     "No permission to create %s", mt->save_path);
        } else {
            snprintf(mt->error_msg, sizeof(mt->error_msg),
                     "Could not create %s: %s",
                     mt->save_path, strerror(saved_errno));
        }
        return -1;
    }

    /* For magnet links without metadata, we need to do metadata download first */
    if (mt->is_magnet && mt->torrent->num_pieces == 0) {
        /* For now, mark as error - magnet metadata download
         * requires BEP-9 (extension protocol) */
        mt->state = TORRENT_ERROR;
        mt->error_type = TERR_PARSE_FAILED;
        strncpy(mt->error_code, "magnet_no_metadata", sizeof(mt->error_code) - 1);
        snprintf(mt->error_msg, sizeof(mt->error_msg),
                 "Magnet links require metadata download (BEP-9) - "
                 "use a .torrent file instead");
        return -1;
    }

    /* Initialize piece manager */
    piece_mgr_init(&mt->piece_mgr,
                   mt->torrent->num_pieces,
                   mt->torrent->piece_length,
                   mt->torrent->total_size,
                   mt->torrent->pieces);

    /* Buffer de la pieza en curso: se pide por bloques de 16 KB y se acumula
     * aqui hasta tener la pieza completa (para verificar el SHA-1 de una vez). */
    rx_teardown(mt);
    mt->rx_buf = malloc((size_t)mt->torrent->piece_length);
    if (!mt->rx_buf) {
        mt->state = TORRENT_ERROR;
        mt->error_type = TERR_WRITE_FAILED;
        strncpy(mt->error_code, "no_memory", sizeof(mt->error_code) - 1);
        snprintf(mt->error_msg, sizeof(mt->error_msg),
                 "Not enough memory for a %lld byte piece buffer",
                 (long long)mt->torrent->piece_length);
        return -1;
    }
    rx_reset(mt);

    /* Initialize file writer */
    if (file_writer_init(&mt->file_writer, mt->torrent, mt->save_path) < 0) {
        int saved_errno = errno;
        mt->state = TORRENT_ERROR;
        mt->error_type = TERR_WRITE_FAILED;
        strncpy(mt->error_code, "write_failed", sizeof(mt->error_code) - 1);
        snprintf(mt->error_msg, sizeof(mt->error_msg),
                 "Could not create the file in %s: %s",
                 mt->save_path, strerror(saved_errno));
        return -1;
    }

    mt->state = TORRENT_DOWNLOADING;
    mt->start_time = get_time_sec();
    mt->last_announce_time = 0;
    mt->last_speed_calc = get_time_sec();
    mt->last_downloaded = mt->downloaded;
    mt->last_uploaded = mt->uploaded;

    ui_log("start #%d ok: state=downloading, %s", index,
           mt->torrent->announce ? mt->torrent->announce : "no announce url");

    return 0;
}

void torrent_mgr_stop(int index)
{
    if (index < 0 || index >= num_torrents) return;
    managed_torrent_t *mt = &torrents[index];

    mt->state = TORRENT_STOPPED;

    dht_unwant(mt->info_hash);

    /* Close all peer connections */
    for (int i = 0; i < mt->num_peers; i++) {
        if (mt->peers[i].sock >= 0) {
            net_close(mt->peers[i].sock);
            mt->peers[i].sock = -1;
        }
        free(mt->peers[i].bitfield);
        mt->peers[i].bitfield = NULL;
        mt->peers[i].bitfield_len = 0;
        mt->peers[i].has_bitfield = 0;
    }
    mt->num_peers = 0;
    mt->active_peers = 0;
    mt->peer_pool_count = 0;
    mt->speed_down = 0;
    mt->speed_up = 0;

    rx_drop(mt);           /* La pieza a medias vuelve a la cola */
    file_writer_finish(&mt->file_writer);
}

void torrent_mgr_remove(int index)
{
    if (index < 0 || index >= num_torrents) return;

    torrent_mgr_stop(index);

    managed_torrent_t *mt = &torrents[index];

    piece_mgr_destroy(&mt->piece_mgr);
    file_writer_destroy(&mt->file_writer);
    rx_teardown(mt);
    torrent_free(mt->torrent);
    mt->torrent = NULL;

    /* Shift remaining torrents */
    for (int i = index; i < num_torrents - 1; i++) {
        torrents[i] = torrents[i + 1];
    }
    num_torrents--;
    memset(&torrents[num_torrents], 0, sizeof(managed_torrent_t));
}

managed_torrent_t *torrent_mgr_get(int index)
{
    if (index < 0 || index >= num_torrents) return NULL;
    return &torrents[index];
}

int torrent_mgr_count(void)
{
    return num_torrents;
}

int torrent_mgr_capacity(void)
{
    return MAX_TORRENTS;
}

int torrent_mgr_find(const unsigned char info_hash[20])
{
    for (int i = 0; i < num_torrents; i++) {
        if (memcmp(torrents[i].info_hash, info_hash, 20) == 0)
            return i;
    }
    return -1;
}

const char *torrent_mgr_error_str(torrent_error_t err)
{
    switch (err) {
    case TERR_NONE:          return "No error";
    case TERR_PARSE_FAILED:  return "Parse failed";
    case TERR_TRACKER_FAILED:return "Tracker failed";
    case TERR_NO_PEERS:      return "No peers found";
    case TERR_WRITE_FAILED:  return "Write failed";
    case TERR_HASH_MISMATCH: return "Hash mismatch";
    default:                 return "Unknown error";
    }
}

/**
 * Pool de direcciones candidatas.
 *
 * Los peers ya no salen solo del announce: PEX (BEP-11) los trae a decenas. Eso
 * obliga a guardarlos y conectarlos de poco en poco, porque cada connect bloquea
 * hasta NET_PEER_CONNECT_TIMEOUT_SEC y el panel comparte este hilo.
 *
 * @return 1 si la direccion era nueva
 */
static int peer_pool_add(managed_torrent_t *mt, const peer_addr_t *addr)
{
    if (mt->peer_pool_count >= MAX_PEER_POOL)
        return 0;

    for (int i = 0; i < mt->peer_pool_count; i++) {
        if (mt->peer_pool[i].addr.ip == addr->ip &&
            mt->peer_pool[i].addr.port == addr->port)
            return 0;
    }

    mt->peer_pool[mt->peer_pool_count].addr = *addr;
    mt->peer_pool[mt->peer_pool_count].next_try = 0;
    mt->peer_pool_count++;
    return 1;
}

/**
 * Conecta con una direccion candidata y, si sale bien, la convierte en peer.
 *
 * @return 1 peer nuevo conectado, 0 fallo de red, -1 nada que hacer (ya estaba
 *         conectado o la tabla de peers esta llena)
 */
static int connect_one(managed_torrent_t *mt, const peer_addr_t *addr)
{
    int peer_slot = -1;
    int already = 0;

    /* Reutilizar el hueco de un peer caido con la misma direccion y no abrir
     * un socket duplicado si ya esta activo. */
    for (int j = 0; j < mt->num_peers; j++) {
        if (mt->peers[j].addr.ip == addr->ip &&
            mt->peers[j].addr.port == addr->port) {
            if (mt->peers[j].sock >= 0)
                already = 1;
            else
                peer_slot = j;
            break;
        }
    }
    if (already)
        return -1;
    if (peer_slot < 0 && mt->num_peers >= MAX_PEERS_PER_TORRENT)
        return -1;

    int sock = net_tcp_connect_timeout(addr->ip, ntohs(addr->port),
                                       NET_PEER_CONNECT_TIMEOUT_SEC);
    if (sock < 0) {
        /* Se registra la direccion: sirve para distinguir peers de IPv6
         * (inalcanzables desde aqui) de peers caidos. */
        ui_log("peer connect failed: %u.%u.%u.%u:%u",
               (ntohl(addr->ip) >> 24) & 0xff,
               (ntohl(addr->ip) >> 16) & 0xff,
               (ntohl(addr->ip) >> 8) & 0xff,
               ntohl(addr->ip) & 0xff,
               ntohs(addr->port));
        return 0;
    }

    /* El handshake tiene su propio presupuesto: hay peers que aceptan TCP y
     * luego se quedan callados, y con el timeout largo de E/S el panel se
     * quedaba muerto 8 s por cada uno. */
    net_set_timeout(sock, NET_PEER_HANDSHAKE_TIMEOUT_SEC);

    uint8_t remote_id[20];
    uint8_t peer_reserved[8];
    if (peer_handshake(sock, mt->torrent->info_hash,
                       mt->peer_id, remote_id, peer_reserved) < 0) {
        net_close(sock);
        return 0;
    }

    net_set_timeout(sock, NET_PEER_IO_TIMEOUT_SEC);

    if (peer_slot < 0)
        peer_slot = mt->num_peers++;
    torrent_peer_t *p = &mt->peers[peer_slot];
    p->sock = sock;
    p->addr = *addr;
    memcpy(p->peer_id, remote_id, 20);
    p->choked = 1;
    p->am_interested = 0;
    p->last_request = 0;
    p->messages_seen = 0;
    p->last_keepalive = get_time_sec();
    p->downloaded = 0;
    p->uploaded = 0;
    p->ext_id_ut_pex = 0;
    p->last_pex_sent = 0;
    p->supports_ext = (peer_reserved[5] & 0x10) != 0;

    /* Bitfield del peer: se reserva ya para poder copiarlo en cuanto llegue
     * (es lo que permite pedirle solo lo que tiene). */
    if (!p->bitfield) {
        p->bitfield_len = (mt->torrent->num_pieces + 7) / 8;
        p->bitfield = calloc(p->bitfield_len, 1);
    } else {
        memset(p->bitfield, 0, p->bitfield_len);
    }
    p->has_bitfield = 0;

    mt->active_peers++;

    peer_send_interested(sock);
    /* BEP-10: el handshake extendido se manda SOLO a quien lo ha anunciado en
     * los reservados. A un peer sin el bit extendido, un mensaje de extension
     * (id 20) le parece una violacion del protocolo y cierra la conexion: era
     * el motivo de que los peers desaparecieran al instante. */
    if (p->supports_ext) {
        peer_send_extended_handshake(sock);
    } else {
        ui_log("peer %d does not support the extension protocol (no PEX)",
               peer_slot);
    }

    return 1;
}

/**
 * Prueba direcciones del pool dentro de un presupuesto de tiempo.
 *
 * @param deadline       Segundos (epoch) en los que hay que parar
 * @param max_connects   Maximo de peers nuevos por ronda
 * @param failed         Salida: intentos que fallaron
 * @return peers conectados en esta ronda
 */
static int connect_from_pool(managed_torrent_t *mt, uint64_t deadline,
                             int max_connects, int *failed)
{
    int made = 0;
    uint64_t now = get_time_sec();

    for (int i = 0; i < mt->peer_pool_count && made < max_connects; ) {
        if (get_time_sec() >= deadline)
            break;

        if (mt->peer_pool[i].next_try > now) {
            i++;
            continue;
        }

        peer_addr_t addr = mt->peer_pool[i].addr;
        int ret = connect_one(mt, &addr);

        if (ret != 0) {
            /* Conectado o descartado (ya estaba / no cabe): fuera del pool. */
            mt->peer_pool[i] = mt->peer_pool[--mt->peer_pool_count];
            if (ret == 1)
                made++;
        } else {
            /* Fallo de red: no insistir en cada vuelta */
            mt->peer_pool[i].next_try = now + PEER_POOL_RETRY_SEC;
            if (failed)
                (*failed)++;
            i++;
        }
    }

    return made;
}

/**
 * Callback de la DHT: un peer nuevo para uno de los torrents activos.
 *
 * La DHT no sabe de torrents; solo devuelve (info_hash, direccion). Aqui se
 * traduce a un torrent concreto y se mete en su pool de candidatos, el mismo
 * que alimenta el tracker y PEX.
 */
void torrent_mgr_on_dht_peer(const unsigned char info_hash[20],
                             const peer_addr_t *peer)
{
    int idx;

    if (!peer)
        return;

    idx = torrent_mgr_find(info_hash);
    if (idx < 0)
        return;

    peer_pool_add(&torrents[idx], peer);
}

/**
 * Announce to tracker for a specific torrent.
 */
static int announce_to_tracker(managed_torrent_t *mt)
{
    if (!mt->torrent->announce && mt->torrent->announce_list_count == 0)
        return -1;

    tracker_params_t params;
    params.info_hash = mt->torrent->info_hash;
    params.peer_id = mt->peer_id;
    params.port = 6881;
    params.uploaded = (int64_t)mt->uploaded;
    params.downloaded = (int64_t)mt->downloaded;
    params.left = (int64_t)(mt->total_size - mt->downloaded);
    params.compact = 1;

    int contacted = 0;
    int peers_reported = 0;
    /* A blocking connect can cost up to NET_PEER_CONNECT_TIMEOUT_SEC, and the
     * web panel shares this thread, so the number of new peers tried per
     * announce is bounded. The rest are retried on the next announce. */
    int connects_this_round = 0;
    int connects_ok = 0;
    int connects_failed = 0;
    int tried_trackers = 0;
    int skipped_trackers = 0;
    uint64_t now = get_time_sec();
    uint64_t deadline = now + ANNOUNCE_BUDGET_SEC;

    mt->tracker_announces++;
    mt->tracker_interval = 30;

    /* Trackers del .torrent + los extra de tracker_extra_list(). Con el cursor
     * por rondas, todos acaban probandose: si no, los dos primeros que contesten
     * bien acaparan el turno para siempre. */
    size_t own_count = mt->torrent->announce_list_count +
                       (mt->torrent->announce ? 1u : 0u);
    size_t extra_count = 0;
    const char *const *extras = tracker_extra_list(&extra_count);
    size_t url_count = own_count + extra_count;

    for (size_t k = 0; k < url_count; k++) {
        size_t url_index = (mt->announce_cursor + k) % url_count;
        const char *url;

        if (url_index < own_count) {
            url = url_index == 0 && mt->torrent->announce
                ? mt->torrent->announce
                : mt->torrent->announce_list[
                    url_index - (mt->torrent->announce ? 1u : 0u)];
        } else {
            url = extras[url_index - own_count];
        }

        if (!url) continue;
        if (strncmp(url, "http://", 7) != 0 &&
            strncmp(url, "https://", 8) != 0 &&
            strncmp(url, "udp://", 6) != 0)
            continue;
        if (mt->torrent->announce && url != mt->torrent->announce &&
            strcmp(url, mt->torrent->announce) == 0) continue;

        /* A tracker that failed recently is skipped until its backoff expires.
         * Skipping is free, so a list full of dead trackers costs nothing. */
        if (url_index < MAX_TRACKER_STATE &&
            mt->tracker_state[url_index].next_try_sec > now) {
            skipped_trackers++;
            continue;
        }

        /* Bounded work per round: the web panel shares this thread. */
        if (tried_trackers >= MAX_TRACKERS_PER_ANNOUNCE ||
            get_time_sec() >= deadline) {
            /* La proxima ronda empieza donde nos hemos quedado. */
            mt->announce_cursor = (url_index + 1) % url_count;
            break;
        }
        tried_trackers++;
        mt->announce_cursor = (url_index + 1) % url_count;

        tracker_response_t *tr = tracker_announce(url, &params);

        if (url_index < MAX_TRACKER_STATE) {
            if (tr) {
                mt->tracker_state[url_index].fails = 0;
                mt->tracker_state[url_index].next_try_sec = 0;
            } else {
                int fails = ++mt->tracker_state[url_index].fails;
                uint64_t backoff = (uint64_t)fails * TRACKER_BACKOFF_STEP_SEC;
                if (backoff > MAX_TRACKER_BACKOFF_SEC)
                    backoff = MAX_TRACKER_BACKOFF_SEC;
                mt->tracker_state[url_index].next_try_sec = now + backoff;
                ui_log("announce: tracker %zu did not answer (%d time(s)); "
                       "backoff %llus", url_index, fails,
                       (unsigned long long)backoff);
            }
        }

        if (!tr) continue;
        contacted++;
        if (tr->failure_reason) {
            tracker_response_free(tr);
            continue;
        }
        if (tr->interval > mt->tracker_interval)
            mt->tracker_interval = tr->interval;
        peers_reported += tr->num_peers;

        /* Los peers del tracker van al pool: el de Ubuntu solo manda UNO por
         * vuelta, asi que hay que juntarlos con los que traiga PEX. */
        if (tr->num_peers > 0 && tr->peers) {
            for (int i = 0; i < tr->num_peers; i++)
                peer_pool_add(mt, &tr->peers[i]);

            int failed = 0;
            int made = connect_from_pool(mt, deadline,
                                         MAX_PEER_CONNECTS_PER_ANNOUNCE,
                                         &failed);
            connects_this_round += made;
            connects_ok += made;
            connects_failed += failed;
        }
        tracker_response_free(tr);
    }

    if (mt->active_peers > 0) {
        mt->error_type = TERR_NONE;
        mt->error_msg[0] = '\0';
        mt->error_code[0] = '\0';
        ui_log("announce ok: %d tracker(s), %d peer(s) offered, %d connected "
               "(%d failed, active=%d, %d tracker(s) in backoff)", contacted,
               peers_reported, connects_ok, connects_failed, mt->active_peers,
               skipped_trackers);
        return 0;
    }

    if (contacted == 0) {
        strncpy(mt->error_code, "no_tracker", sizeof(mt->error_code) - 1);
        snprintf(mt->error_msg, sizeof(mt->error_msg),
                 "No tracker answered; see the log for the reason");
    } else if (peers_reported == 0) {
        strncpy(mt->error_code, "no_peers", sizeof(mt->error_code) - 1);
        snprintf(mt->error_msg, sizeof(mt->error_msg),
                 "Trackers answered, but no peers are available; will retry automatically");
    } else {
        strncpy(mt->error_code, "peers_failed", sizeof(mt->error_code) - 1);
        snprintf(mt->error_msg, sizeof(mt->error_msg),
                 "Peers were found, but the connections failed; will retry automatically");
    }
    mt->error_type = TERR_NO_PEERS;
    ui_log("announce failed: %s (trackers_tried=%d ok=%d peers_offered=%d "
           "peer_connects=%d failed=%d): %s", mt->error_code, tried_trackers,
           contacted, peers_reported, connects_this_round, connects_failed,
           mt->error_msg);
    return -1;
}

/**
 * Mensajes de la extension (BEP-10).
 *
 * De momento solo ut_pex (BEP-11), pero es justo lo que hacia falta: el tracker
 * de Ubuntu entrega UN peer por announce y el enjambre tiene cientos. El peer
 * conectado nos pasa su lista y con eso el enjambre se construye en minutos.
 */
static void peer_handle_extended(managed_torrent_t *mt, int peer_idx,
                                 const peer_msg_t *msg, const unsigned char *payload)
{
    torrent_peer_t *p = &mt->peers[peer_idx];

    if (msg->ext_id == PEER_EXT_HANDSHAKE) {
        bcode_node_t *root = bcode_parse(payload, msg->data_len);
        bcode_node_t *m;
        bcode_node_t *pex;

        if (!root) {
            ui_log("peer %d: extended handshake not understood", peer_idx);
            return;
        }

        m = bcode_dict_get(root, "m");
        pex = m ? bcode_dict_get(m, "ut_pex") : NULL;
        if (pex) {
            int64_t id = bcode_int_val(pex);
            if (id > 0 && id < 256) {
                p->ext_id_ut_pex = (uint8_t)id;
                ui_log("peer %d supports ut_pex (id %d)", peer_idx, (int)id);
            }
        }
        bcode_free(root);
        return;
    }

    if (msg->ext_id != PEER_EXT_UT_PEX)
        return;

    bcode_node_t *root = bcode_parse(payload, msg->data_len);
    if (!root)
        return;

    bcode_node_t *added = bcode_dict_get(root, "added");
    size_t added_len = 0;
    const unsigned char *added_buf = added ? bcode_str_val(added, &added_len) : NULL;

    if (added_buf) {
        int fresh = 0;

        for (size_t off = 0; off + 6 <= added_len; off += 6) {
            peer_addr_t addr;

            memcpy(&addr.ip, added_buf + off, 4);
            memcpy(&addr.port, added_buf + off + 4, 2);

            if (addr.port != 0 && peer_pool_add(mt, &addr))
                fresh++;
        }

        if (fresh > 0)
            ui_log("peer %d ut_pex: %d new peer(s) (pool=%d)",
                   peer_idx, fresh, mt->peer_pool_count);
    }

    bcode_free(root);
}

/**
 * Process one peer for a torrent - try to recv messages and send requests.
 */
static void process_peer(managed_torrent_t *mt, int peer_idx)
{
    torrent_peer_t *p = &mt->peers[peer_idx];
    static unsigned char block_buf[PEER_BLOCK_SIZE];
    peer_msg_t msg;
    int ret;

    if (p->sock < 0) return;

    memset(&msg, 0, sizeof(msg));
    msg.bitfield = p->bitfield;
    msg.bitfield_size = p->bitfield_len;

    ret = peer_recv_message(p->sock, &msg, block_buf, sizeof(block_buf), 0);

    if (ret < 0) {
        ui_log("peer %d disconnected (last msg id %u, errno %d '%s')",
               peer_idx, (unsigned)msg.msg_id, errno, strerror(errno));
        peer_close(mt, peer_idx);
        return;
    }

    switch (msg.msg_id) {
    case PEER_MSG_CHOKE:
        p->choked = 1;
        return;

    case PEER_MSG_UNCHOKE:
        p->choked = 0;
        ui_log("peer %d unchoked us%s", peer_idx,
               p->has_bitfield ? "" : " (bitfield pending)");
        return;

    case PEER_MSG_BITFIELD: {
        size_t have = 0;
        for (size_t i = 0; i < mt->torrent->num_pieces && p->bitfield; i++) {
            if (p->bitfield[i / 8] & (1u << (7 - (i % 8))))
                have++;
        }
        p->has_bitfield = 1;
        p->messages_seen++;
        ui_log("peer %d bitfield: %zu/%zu pieces", peer_idx, have,
               mt->torrent->num_pieces);
        return;
    }

    case PEER_MSG_HAVE:
        if (p->bitfield && msg.piece_index < mt->torrent->num_pieces)
            p->bitfield[msg.piece_index / 8] |=
                (unsigned char)(1u << (7 - (msg.piece_index % 8)));

        /* Un HAVE demuestra que ya sabemos algo de lo que tiene: sin marcarlo
         * aqui, un peer que no manda bitfield (un leecher) se quedaria fuera
         * de la seleccion para siempre. */
        if (!p->has_bitfield) {
            p->has_bitfield = 1;
            ui_log("peer %d has no bitfield; using its HAVE messages instead",
                   peer_idx);
        }

        if (p->messages_seen < 5) {
            p->messages_seen++;
            ui_log("peer %d have piece %u", peer_idx, msg.piece_index);
        }
        return;

    case 0xff:
        return;  /* Keep-alive */

    case 9:
        /* PORT (BEP-5): el peer dice en que puerto UDP escucha su nodo DHT.
         * No hablamos DHT con los peers, asi que se ignora en silencio en vez
         * de ensuciar el log con "msg id 9". */
        return;

    case 13:
        return;  /* SUGGEST (BEP-3 incremental): tampoco se usa */

    case PEER_MSG_EXTENDED:
        peer_handle_extended(mt, peer_idx, &msg, block_buf);
        return;

    default:
        ui_log("peer %d msg id %u", peer_idx, (unsigned)msg.msg_id);
        return;

    case PEER_MSG_PIECE:
        break;
    }

    /* Bloque de datos: se copia en su offset de la pieza en curso. */
    if (ret == 0 && msg.data_len > 0) {
        if (p->sock < 0) return;
        if (msg.piece_index != (uint32_t)mt->rx_piece || !mt->rx_buf) {
            /* De otra pieza (peticion antigua o de otro peer): se descarta */
            return;
        }
        if ((uint64_t)msg.piece_begin + msg.data_len > mt->rx_length)
            return;
        if (mt->rx_pending > 0)
            mt->rx_pending--;

        memcpy(mt->rx_buf + msg.piece_begin, block_buf, msg.data_len);
        mt->rx_have += msg.data_len;
        p->downloaded += msg.data_len;
        mt->rx_blocks++;

        if (mt->rx_blocks <= 3 || mt->rx_blocks % 200 == 0) {
            ui_log("block %zu: piece %u begin %u len %u (%u/%u of piece)",
                   mt->rx_blocks, msg.piece_index, msg.piece_begin,
                   msg.data_len, mt->rx_have, mt->rx_length);
        }

        if (mt->rx_have < mt->rx_length)
            return;  /* Pieza incompleta: faltan bloques */

        /* Pieza completa: verificar SHA-1 y escribir de una vez */
        if (piece_mgr_complete(&mt->piece_mgr, (size_t)mt->rx_piece,
                               mt->rx_buf, mt->rx_length) == 0) {
            file_writer_write_piece(&mt->file_writer, mt->rx_buf,
                                    mt->rx_length, (size_t)mt->rx_piece);
            mt->downloaded += mt->rx_length;
            mt->rx_pieces_done++;
            if (mt->rx_pieces_done % 20 == 0 || mt->rx_pieces_done == 1) {
                ui_log("piece %zu/%zu ok (%.1f MB downloaded)",
                       mt->rx_pieces_done, mt->torrent->num_pieces,
                       (double)mt->downloaded / (1024.0 * 1024.0));
            }
        } else {
            ui_log("piece %d failed SHA-1; retrying", mt->rx_piece);
            piece_mgr_failed(&mt->piece_mgr, (size_t)mt->rx_piece);
        }
        rx_reset(mt);
    }
}

/**
 * Pide bloques por adelantado.
 *
 * El protocolo no admite bloques mayores de 16 KB, asi que una pieza se pide
 * en varios bloques: se elige la siguiente pieza pendiente, se pide de uno en
 * uno hasta llenar la ventana (PEER_MAX_REQUESTS) y se va pidiendo mas segun
 * llegan. Si el peer que la servia se cae o nos bloquea, la pieza se descarta y
 * se reintenta desde cero.
 */
static void peer_pump(managed_torrent_t *mt)
{
    if (!mt->rx_buf || mt->state != TORRENT_DOWNLOADING)
        return;

    /* Pieza en curso cuyo peer ya no sirve: se devuelve a la cola */
    if (mt->rx_piece >= 0) {
        torrent_peer_t *serving = mt->rx_peer >= 0 ? &mt->peers[mt->rx_peer] : NULL;
        if (!serving || serving->sock < 0 || serving->choked) {
            rx_drop(mt);
        }
    }

    /* Elegir pieza y peer si no hay ninguna en curso. Solo se pide a un peer
     * que ha anunciado lo que tiene: pedirle una pieza que no posee es una
     * violacion del protocolo y los clientes desconectan por ello. */
    if (mt->rx_piece < 0) {
        int candidate = -1;
        int candidate_piece = -1;

        for (int i = 0; i < mt->num_peers; i++) {
            torrent_peer_t *peer = &mt->peers[i];
            int piece;

            if (peer->sock < 0 || peer->choked || !peer->has_bitfield)
                continue;

            piece = piece_mgr_get_next(&mt->piece_mgr, peer->bitfield,
                                       peer->bitfield_len);
            if (piece >= 0) {
                candidate = i;
                candidate_piece = piece;
                break;
            }
        }

        if (candidate < 0)
            return;  /* Ningun peer disponible con piezas que necesitemos */

        mt->rx_piece = candidate_piece;
        mt->rx_length = (uint32_t)mt->torrent->piece_length;
        if ((size_t)candidate_piece == mt->torrent->num_pieces - 1) {
            mt->rx_length = (uint32_t)(mt->torrent->total_size -
                              (int64_t)candidate_piece * mt->torrent->piece_length);
        }
        mt->rx_have = 0;
        mt->rx_next = 0;
        mt->rx_pending = 0;
        mt->rx_peer = candidate;
        piece_mgr_request(&mt->piece_mgr, (size_t)candidate_piece);
    }

    /* Llenar la ventana de peticiones */
    while (mt->rx_pending < PEER_MAX_REQUESTS && mt->rx_next < mt->rx_length) {
        uint32_t remaining = mt->rx_length - mt->rx_next;
        uint32_t len = remaining > PEER_BLOCK_SIZE ? PEER_BLOCK_SIZE : remaining;
        torrent_peer_t *p = &mt->peers[mt->rx_peer];

        if (peer_send_request(p->sock, (uint32_t)mt->rx_piece, mt->rx_next,
                              len) < 0) {
            rx_drop(mt);
            return;
        }
        p->last_request = (uint32_t)mt->rx_piece;
        mt->rx_next += len;
        mt->rx_pending++;
    }
}

int torrent_mgr_tick(void)
{
    int active = 0;
    uint64_t now = get_time_sec();

    for (int i = 0; i < num_torrents; i++) {
        managed_torrent_t *mt = &torrents[i];
        if (mt->state != TORRENT_DOWNLOADING) continue;

        active++;

        /* Check if complete */
        if (piece_mgr_is_done(&mt->piece_mgr)) {
            file_writer_finish(&mt->file_writer);
            mt->state = TORRENT_DONE;
            mt->progress = 1.0f;
            mt->speed_down = 0;
            mt->speed_up = 0;
            ui_log("torrent #%d complete: %s (%.1f MB)",
                   i, mt->name, (double)mt->downloaded / (1024.0 * 1024.0));
            dht_unwant(mt->info_hash);
            rx_teardown(mt);
            continue;
        }

        /* Update progress */
        mt->progress = piece_mgr_progress(&mt->piece_mgr);

        /* Calculate speed */
        if (now > mt->last_speed_calc) {
            uint64_t elapsed = now - mt->last_speed_calc;
            if (elapsed >= 2) {
                mt->speed_down = (mt->downloaded - mt->last_downloaded) / elapsed;
                mt->speed_up = (mt->uploaded - mt->last_uploaded) / elapsed;
                mt->last_speed_calc = now;
                mt->last_downloaded = mt->downloaded;
                mt->last_uploaded = mt->uploaded;
            }
        }

        /* Re-announce. El intervalo que pide el tracker (1800 s en el de
         * Ubuntu) es demasiado largo mientras nos faltan peers: ese tracker
         * entrega UN peer por vuelta, asi que esperar media hora para pedir
         * otro no construye enjambre ninguno. Con pocos peers se vuelve antes. */
        {
            uint64_t interval = (uint64_t)mt->tracker_interval;

            if (mt->active_peers == 0)
                interval = 30;
            else if (mt->active_peers < 4 && (interval == 0 || interval > 120))
                interval = 120;

            if (interval > 0 && now - mt->last_announce_time >= interval) {
                announce_to_tracker(mt);
                mt->last_announce_time = now;
            }
        }

        /* Process peers with non-blocking reads. Se drena todo lo disponible:
         * un mensaje por vuelta ataba la descarga a la cadencia del bucle y
         * dejaba bloques esperando en el socket. */
        for (int p = 0; p < mt->num_peers; p++) {
            if (mt->peers[p].sock < 0)
                continue;

            for (int guard = 0; guard < 64; guard++) {
                struct timeval tv = {0, 0};
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(mt->peers[p].sock, &readfds);

                if (select(mt->peers[p].sock + 1, &readfds, NULL, NULL, &tv) <= 0)
                    break;

                process_peer(mt, p);
                if (mt->peers[p].sock < 0)
                    break;
            }
        }

        /* Keep-alive: sin esto un peer que no oye nada durante dos minutos nos
         * cierra la conexion. Con un solo peer, perderlo es quedarse parado. */
        for (int p = 0; p < mt->num_peers; p++) {
            torrent_peer_t *peer = &mt->peers[p];

            if (peer->sock < 0 || now - peer->last_keepalive < 120)
                continue;

            if (peer_send_keepalive(peer->sock) < 0) {
                ui_log("peer %d keep-alive failed", p);
                peer_close(mt, p);
                continue;
            }
            peer->last_keepalive = now;
        }

        /* PEX (BEP-11): mandar a cada peer los peers que conocemos. Es como se
         * construye el enjambre cuando el tracker solo da uno por vuelta. */
        for (int p = 0; p < mt->num_peers; p++) {
            torrent_peer_t *peer = &mt->peers[p];
            unsigned char compact[6 * 64];
            int n = 0;

            if (peer->sock < 0 || peer->ext_id_ut_pex == 0)
                continue;
            if (peer->last_pex_sent != 0 && now - peer->last_pex_sent < 60)
                continue;

            for (int q = 0; q < mt->num_peers && n < 64; q++) {
                if (mt->peers[q].sock < 0)
                    continue;
                memcpy(compact + n * 6, &mt->peers[q].addr.ip, 4);
                memcpy(compact + n * 6 + 4, &mt->peers[q].addr.port, 2);
                n++;
            }

            /* Lista vacia: no se manda. Un "added" sin peers no aporta nada y
             * algunos clientes lo cuentan como PEX mal formado. */
            if (n > 0 &&
                peer_send_ut_pex(peer->sock, peer->ext_id_ut_pex,
                                 compact, (size_t)n) == 0)
                peer->last_pex_sent = now;
        }

        /* Candidatos del pool (del tracker y de PEX). Se van probando de poco
         * en poco porque cada connect bloquea y el panel comparte el hilo. Con
         * 10 peers conectados ya hay con quien tirar: mas solo alargaria las
         * pausas del panel para nada. */
        if (mt->active_peers < 10 && mt->peer_pool_count > 0 &&
            now - mt->last_pool_connect >= 5) {
            int failed = 0;

            mt->last_pool_connect = now;
            connect_from_pool(mt, now + POOL_CONNECT_BUDGET_SEC, 4, &failed);
        }

        /* Pedir mas bloques: sin esto solo se pediria al recibir algo */
        peer_pump(mt);
    }

    return active;
}

typedef struct {
    char   *buf;
    size_t  size;
    size_t  pos;
} json_writer_t;

static void json_writef(json_writer_t *writer, const char *fmt, ...)
{
    if (!writer || writer->pos >= writer->size) return;

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(writer->buf + writer->pos,
                            writer->size - writer->pos, fmt, args);
    va_end(args);

    if (written < 0) return;
    if ((size_t)written >= writer->size - writer->pos) {
        writer->pos = writer->size - 1;
        writer->buf[writer->pos] = '\0';
        return;
    }
    writer->pos += (size_t)written;
}

static void json_write_string(json_writer_t *writer, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(value ? value : "");

    json_writef(writer, "\"");
    while (*p && writer->pos + 7 < writer->size) {
        unsigned char c = *p++;
        if (c == '\"' || c == '\\') {
            json_writef(writer, "\\%c", c);
        } else if (c == '\b') {
            json_writef(writer, "\\b");
        } else if (c == '\f') {
            json_writef(writer, "\\f");
        } else if (c == '\n') {
            json_writef(writer, "\\n");
        } else if (c == '\r') {
            json_writef(writer, "\\r");
        } else if (c == '\t') {
            json_writef(writer, "\\t");
        } else if (c < 0x20) {
            char escaped[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
            json_writef(writer, "%s", escaped);
        } else {
            json_writef(writer, "%c", c);
        }
    }
    json_writef(writer, "\"");
}

void torrent_mgr_status_json(char *buf, size_t bufsz)
{
    if (!buf || bufsz < 64) return;

    json_writer_t writer = {buf, bufsz, 0};
    uint64_t now = get_time_sec();
    json_writef(&writer, "{\"torrents\":[");

    for (int i = 0; i < num_torrents; i++) {
        managed_torrent_t *mt = &torrents[i];

        if (i > 0)
            json_writef(&writer, ",");

        const char *state_str = "stopped";
        switch (mt->state) {
        case TORRENT_STOPPED:     state_str = "stopped"; break;
        case TORRENT_DOWNLOADING: state_str = "downloading"; break;
        case TORRENT_SEEDING:     state_str = "seeding"; break;
        case TORRENT_ERROR:       state_str = "error"; break;
        case TORRENT_DONE:        state_str = "done"; break;
        }

        uint64_t remaining = mt->total_size > mt->downloaded
                           ? mt->total_size - mt->downloaded : 0;
        uint64_t elapsed = mt->start_time > 0 && now >= mt->start_time
                         ? now - mt->start_time : 0;
        size_t pieces_total = mt->torrent ? mt->torrent->num_pieces : 0;
        size_t pieces_done = pieces_total ? mt->piece_mgr.num_complete : 0;

        json_writef(&writer, "{\"id\":%d,\"name\":", i);
        json_write_string(&writer, mt->name);
        json_writef(&writer, ",\"hash\":");
        json_write_string(&writer, mt->id);
        json_writef(&writer,
            ",\"state\":\"%s\","
            "\"progress\":%.3f,"
            "\"size\":%llu,"
            "\"downloaded\":%llu,"
            "\"uploaded\":%llu,"
            "\"remaining\":%llu,"
            "\"speed_down\":%llu,"
            "\"speed_up\":%llu,"
            "\"peers\":%d,"
            "\"active_peers\":%d,"
            "\"pieces_done\":%zu,"
            "\"pieces_total\":%zu,"
            "\"elapsed\":%llu,"
            "\"tracker_announces\":%d,"
            "\"is_magnet\":%s,"
            "\"save_path\":",
            state_str,
            (double)mt->progress,
            (unsigned long long)mt->total_size,
            (unsigned long long)mt->downloaded,
            (unsigned long long)mt->uploaded,
            (unsigned long long)remaining,
            (unsigned long long)mt->speed_down,
            (unsigned long long)mt->speed_up,
            mt->num_peers,
            mt->active_peers,
            pieces_done,
            pieces_total,
            (unsigned long long)elapsed,
            mt->tracker_announces,
            mt->is_magnet ? "true" : "false");
        json_write_string(&writer, mt->save_path);
        json_writef(&writer, ",\"error\":");
        json_write_string(&writer, mt->error_msg[0] ? mt->error_msg : "");
        json_writef(&writer, ",\"error_code\":");
        json_write_string(&writer, mt->error_code[0] ? mt->error_code : "");
        json_writef(&writer, "}");
    }

    json_writef(&writer, "]}");
}

/**
 * Log de progreso, una linea por torrent. Es la traza que de verdad importa
 * durante el uso normal: cuanto llevamos, a que velocidad y con cuantos peers.
 */
void torrent_mgr_log_progress(void)
{
    char torrent_id[64];
    for (int i = 0; i < num_torrents; i++) {
        managed_torrent_t *mt = &torrents[i];
        const char *state = "?";
        switch (mt->state) {
        case TORRENT_STOPPED:     state = "stopped"; break;
        case TORRENT_DOWNLOADING: state = "downloading"; break;
        case TORRENT_SEEDING:     state = "seeding"; break;
        case TORRENT_ERROR:       state = "error"; break;
        case TORRENT_DONE:        state = "done"; break;
        }
        snprintf(torrent_id, sizeof(torrent_id), "%s",
                 mt->name[0] ? mt->name : "(sin nombre)");
        ui_log("progress: #%d '%s' %s %.2f%% (%.1f/%.1f MB) %.0f KB/s peers=%d",
               i, torrent_id, state,
               (double)mt->progress * 100.0,
               (double)mt->downloaded / (1024.0 * 1024.0),
               (double)mt->total_size / (1024.0 * 1024.0),
               (double)mt->speed_down / 1024.0,
               mt->active_peers);
    }
}

void torrent_mgr_shutdown(void)
{
    for (int i = num_torrents - 1; i >= 0; i--) {
        torrent_mgr_remove(i);
    }
    num_torrents = 0;
}
