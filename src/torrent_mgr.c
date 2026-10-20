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
#include <sys/socket.h>
#include <sys/select.h>
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

/** Tiempo en milisegundos, para presupuestar cuanto dura una vuelta. */
static uint64_t get_time_ms_local(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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
    mt->selected_size = torrent->total_size;   /* por defecto, todo */
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
 * Reinicia una ranura de descarga.
 */
static void rx_slot_reset(managed_torrent_t *mt, int slot)
{
    mt->rx[slot].piece = -1;
    mt->rx[slot].length = 0;
    mt->rx[slot].have = 0;
    mt->rx[slot].next = 0;
    mt->rx[slot].pending = 0;
    mt->rx[slot].peer = -1;
}

/**
 * Abandona la pieza de una ranura devolviendola a la cola del gestor (si no, se
 * quedaria marcada como pedida y no se repartiria nunca mas).
 */
static void rx_slot_drop(managed_torrent_t *mt, int slot)
{
    if (mt->rx[slot].piece >= 0)
        piece_mgr_failed(&mt->piece_mgr, (size_t)mt->rx[slot].piece);
    rx_slot_reset(mt, slot);
}

/** Ranura que esta descargando esa pieza (-1 si ninguna). */
static int rx_slot_for_piece(const managed_torrent_t *mt, int piece)
{
    for (int s = 0; s < mt->rx_slots; s++) {
        if (mt->rx[s].piece == piece)
            return s;
    }
    return -1;
}

/** Todas las piezas a medias vuelven a la cola. */
static void rx_drop_all(managed_torrent_t *mt)
{
    for (int s = 0; s < mt->rx_slots; s++)
        rx_slot_drop(mt, s);
}

/** Abandona la ranura que servia ese peer (se ha caido o nos ha bloqueado). */
static void rx_drop_peer(managed_torrent_t *mt, int peer_idx)
{
    for (int s = 0; s < mt->rx_slots; s++) {
        if (mt->rx[s].piece >= 0 && mt->rx[s].peer == peer_idx)
            rx_slot_drop(mt, s);
    }
}

/**
 * Libera los buffers de pieza y reinicia el estado de descarga.
 */
static void rx_teardown(managed_torrent_t *mt)
{
    for (int s = 0; s < PEER_INFLIGHT_MAX; s++) {
        free(mt->rx[s].buf);
        mt->rx[s].buf = NULL;
        rx_slot_reset(mt, s);
    }
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

    /* Solo cuenta como peer activo si llego a estar listo: los que se quedan
     * por el camino (connect o handshake fallidos) no deben descontar. */
    if (p->state == PEER_STATE_READY && mt->active_peers > 0)
        mt->active_peers--;

    if (p->sock >= 0)
        net_close(p->sock);

    p->sock = -1;
    p->state = PEER_STATE_IDLE;
    p->hs_received = 0;
    p->ext_id_ut_pex = 0;
    p->supports_ext = 0;
    free(p->bitfield);
    p->bitfield = NULL;
    p->bitfield_len = 0;
    p->has_bitfield = 0;

    if (mt->rx_slots > 0)
        rx_drop_peer(mt, peer_idx);
}

/**
 * Traduce la seleccion de ficheros a piezas saltadas.
 *
 * La verificacion es por pieza entera (SHA-1 del bloque de 256 KB), asi que no
 * se puede "media pieza": una pieza se descarga si la toca al menos un fichero
 * elegido, y se salta solo si no la toca ninguno. Los bytes de un fichero no
 * elegido que caen dentro de una pieza compartida se descartan al escribir.
 */
static void apply_file_selection(managed_torrent_t *mt)
{
    piece_mgr_t *pm = &mt->piece_mgr;
    int64_t piece_len = mt->torrent->piece_length;
    unsigned char *needed;
    int64_t offset = 0;
    int64_t selected = 0;

    if (!mt->file_selected || mt->torrent->num_files == 0 || piece_len <= 0) {
        mt->selected_size = mt->torrent->total_size;
        return;
    }

    needed = calloc(pm->bitfield_len ? pm->bitfield_len : 1, 1);
    if (!needed) {
        /* Sin memoria para el mapa: se descarga todo, que es lo que se hacia
         * antes de existir esta funcion. */
        ui_log("selection: no memory for the piece map; downloading everything");
        mt->selected_size = mt->torrent->total_size;
        return;
    }

    for (size_t i = 0; i < mt->torrent->num_files; i++) {
        int64_t len = mt->torrent->files[i].length;

        if (mt->file_selected[i] && len > 0) {
            int64_t first = offset / piece_len;
            int64_t last = (offset + len - 1) / piece_len;

            selected += len;

            for (int64_t p = first; p <= last && p < (int64_t)pm->num_pieces; p++)
                needed[p >> 3] |= (unsigned char)(1 << (7 - (p & 7)));
        }

        offset += len;
    }

    for (size_t p = 0; p < pm->num_pieces; p++) {
        if (!(needed[p >> 3] & (1 << (7 - (p & 7)))))
            piece_mgr_skip(pm, p);
    }

    free(needed);

    mt->selected_size = selected;
    ui_log("selection: %lld of %lld bytes, %zu of %zu pieces skipped",
           (long long)selected, (long long)mt->torrent->total_size,
           pm->num_skipped, pm->num_pieces);
}

int torrent_mgr_set_selection(int index, const unsigned char *selected,
                              size_t count)
{
    managed_torrent_t *mt;

    if (index < 0 || index >= num_torrents)
        return -1;

    mt = &torrents[index];
    if (!mt->torrent)
        return -1;

    /* Un solo fichero: no hay nada que elegir. */
    if (mt->torrent->num_files == 0)
        return 0;

    if (count != mt->torrent->num_files)
        return -1;

    if (!mt->file_selected) {
        mt->file_selected = malloc(count);
        if (!mt->file_selected)
            return -1;
    }

    memcpy(mt->file_selected, selected, count);
    return 0;
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

    /* Traducir la seleccion de ficheros a piezas saltadas. Se hace antes de
     * arrancar para que el motor no pida lo que el usuario no quiere. */
    apply_file_selection(mt);

    /* Ranuras de descarga: el objetivo es tener muchas piezas en vuelo a la vez
     * (es la palanca de velocidad: con 27 peers conectados y 4 ranuras solo
     * trabajan 4), pero con un presupuesto de memoria. Este torrent tiene piezas
     * de 256 KB, asi que salen 16 ranuras y 4 MB de buffers. */
    rx_teardown(mt);
    mt->rx_slots = (int)((8u * 1024u * 1024u) / (size_t)mt->torrent->piece_length);
    if (mt->rx_slots > PEER_INFLIGHT_MAX)
        mt->rx_slots = PEER_INFLIGHT_MAX;
    if (mt->rx_slots < 1)
        mt->rx_slots = 1;
    rx_drop_all(mt);

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

    mt->file_writer.file_selected = mt->file_selected;

    mt->state = TORRENT_DOWNLOADING;
    mt->start_time = get_time_sec();
    mt->last_announce_time = 0;
    mt->last_speed_calc = get_time_sec();
    mt->last_speed_bytes = mt->bytes_in;
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

    rx_drop_all(mt);       /* Las piezas a medias vuelven a la cola */
    mt->rx_slots = 0;
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
    free(mt->file_selected);
    mt->file_selected = NULL;
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
static int peer_start_connect(managed_torrent_t *mt, const peer_addr_t *addr)
{
    int peer_slot = -1;

    /* Reutilizar el hueco de un peer caido con la misma direccion y no abrir
     * un socket duplicado si ya esta conectando o conectado. */
    for (int j = 0; j < mt->num_peers; j++) {
        if (mt->peers[j].addr.ip == addr->ip &&
            mt->peers[j].addr.port == addr->port) {
            if (mt->peers[j].state != PEER_STATE_IDLE)
                return 1;
            peer_slot = j;
            break;
        }
    }

    if (peer_slot < 0) {
        if (mt->num_peers >= MAX_PEERS_PER_TORRENT) {
            /* Tabla llena: se recicla un hueco libre. Sin esto, tras 50
             * direcciones muertas (lo normal con NAT) no se probaria ninguna
             * mas y el torrente se quedaria clavado. */
            for (int j = 0; j < mt->num_peers; j++) {
                if (mt->peers[j].state == PEER_STATE_IDLE) {
                    peer_slot = j;
                    break;
                }
            }
            if (peer_slot < 0)
                return 1;   /* todos los huecos estan en uso: nada que hacer */
        } else {
            peer_slot = mt->num_peers;
        }
    }

    /* Lanzar la conexion y seguir a lo nuestro: se termina de resolver en
     * peer_step_connecting(). Aqui no se bloquea nada. */
    int sock = net_tcp_connect_start(addr->ip, ntohs(addr->port));
    if (sock < 0)
        return 0;

    if (peer_slot == mt->num_peers)
        mt->num_peers++;

    torrent_peer_t *p = &mt->peers[peer_slot];
    p->sock = sock;
    p->addr = *addr;
    p->state = PEER_STATE_CONNECTING;
    p->deadline = get_time_sec() + NET_PEER_CONNECT_TIMEOUT_SEC;
    p->hs_received = 0;
    p->choked = 1;
    p->am_interested = 0;
    p->last_request = 0;
    p->messages_seen = 0;
    p->last_keepalive = get_time_sec();
    p->last_pex_sent = 0;
    p->downloaded = 0;
    p->uploaded = 0;
    p->connected_since = 0;
    p->last_bytes = 0;
    p->speed = 0;
    p->ext_id_ut_pex = 0;
    p->supports_ext = 0;

    /* Bitfield del peer: se reserva ya para poder copiarlo en cuanto llegue
     * (es lo que permite pedirle solo lo que tiene). */
    if (!p->bitfield) {
        p->bitfield_len = (mt->torrent->num_pieces + 7) / 8;
        p->bitfield = calloc(p->bitfield_len, 1);
    } else {
        memset(p->bitfield, 0, p->bitfield_len);
    }
    p->has_bitfield = 0;

    return 1;
}

/** Peers vivos (los huecos libres de la tabla no cuentan). */
static int count_live_peers(const managed_torrent_t *mt)
{
    int n = 0;

    for (int i = 0; i < mt->num_peers; i++) {
        if (mt->peers[i].state != PEER_STATE_IDLE)
            n++;
    }

    return n;
}

/** Conexiones que aun no han terminado (connect o handshake en curso). */
static int count_inflight(managed_torrent_t *mt)
{
    int n = 0;

    for (int i = 0; i < mt->num_peers; i++) {
        if (mt->peers[i].state == PEER_STATE_CONNECTING ||
            mt->peers[i].state == PEER_STATE_HANDSHAKING)
            n++;
    }

    return n;
}

/**
 * Lanza conexiones no bloqueantes contra direcciones del pool.
 *
 * Ya no hace falta presupuesto de tiempo: lanzar un connect no cuesta nada. El
 * limite es cuantas conexiones puede haber en vuelo, no los segundos. Medido:
 * de 30 candidatos solo 4 aceptan TCP, asi que probarlos rapido y en paralelo es
 * la diferencia entre encontrar peers en segundos o en minutos.
 *
 * @param max_starts  Maximo de conexiones a lanzar en esta vuelta
 * @param failed      Salida: direcciones descartadas por fallo inmediato
 * @return conexiones lanzadas
 */
static int connect_from_pool(managed_torrent_t *mt, int max_starts, int *failed)
{
    int started = 0;
    uint64_t now = get_time_sec();

    for (int i = 0; i < mt->peer_pool_count && started < max_starts; ) {
        peer_addr_t addr;

        if (count_inflight(mt) >= MAX_PEERS_INFLIGHT)
            break;

        if (mt->peer_pool[i].next_try > now) {
            i++;
            continue;
        }

        addr = mt->peer_pool[i].addr;

        if (peer_start_connect(mt, &addr)) {
            /* Lanzada (o ya estaba en curso): fuera del pool. */
            mt->peer_pool[i] = mt->peer_pool[--mt->peer_pool_count];
            started++;
        } else {
            /* Aqui solo fallan los rechazos inmediatos (RST); los timeouts se
             * detectan mas tarde, en peer_step_connecting(). */
            mt->peer_pool[i].next_try = now + PEER_POOL_RETRY_SEC;
            if (failed)
                (*failed)++;
            i++;
        }
    }

    return started;
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
 * Avanza la conexion de un peer que aun no esta listo: connect y handshake, los
 * dos por pasos y sin bloquear nada.
 *
 * Medido el 17-10-2026 sobre 30 candidatos: 26 no aceptan TCP y 3 de los 4 que
 * aceptan se quedan mudos. Siendo bloqueante, cada uno de esos cuesta segundos
 * de panel congelado; aqui cuestan cero, y por eso se pueden probar decenas a
 * la vez.
 */
static void peer_step_connecting(managed_torrent_t *mt, int peer_idx)
{
    torrent_peer_t *p = &mt->peers[peer_idx];
    uint64_t now = get_time_sec();

    if (p->state == PEER_STATE_CONNECTING) {
        int r = net_tcp_connect_poll(p->sock);

        if (r < 0) {
            ui_log("peer connect failed: %u.%u.%u.%u:%u",
                   (ntohl(p->addr.ip) >> 24) & 0xff,
                   (ntohl(p->addr.ip) >> 16) & 0xff,
                   (ntohl(p->addr.ip) >> 8) & 0xff,
                   ntohl(p->addr.ip) & 0xff,
                   ntohs(p->addr.port));
            peer_close(mt, peer_idx);
            return;
        }

        if (r == 0) {
            /* Sigue intentandose. Si se pasa del plazo, se descarta sin traza:
             * son la mayoria y llenarian el log. */
            if (now >= p->deadline)
                peer_close(mt, peer_idx);
            return;
        }

        if (peer_send_handshake(p->sock, mt->torrent->info_hash,
                                mt->peer_id) < 0) {
            peer_close(mt, peer_idx);
            return;
        }

        p->state = PEER_STATE_HANDSHAKING;
        p->hs_received = 0;
        p->deadline = now + NET_PEER_HANDSHAKE_TIMEOUT_SEC;
        return;
    }

    if (p->state != PEER_STATE_HANDSHAKING)
        return;

    /* Leer su handshake sin bloquear: casi siempre cabe en un paquete, pero
     * puede venir troceado. */
    while (p->hs_received < PEER_HANDSHAKE_LEN) {
        ssize_t n = recv(p->sock, p->hs_buf + p->hs_received,
                         (size_t)(PEER_HANDSHAKE_LEN - p->hs_received), 0);

        if (n > 0) {
            p->hs_received += (int)n;
            continue;
        }
        if (n == 0) {
            peer_close(mt, peer_idx);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            peer_close(mt, peer_idx);
            return;
        }
        break;   /* todavia no hay mas */
    }

    if (p->hs_received < PEER_HANDSHAKE_LEN) {
        if (now >= p->deadline)
            peer_close(mt, peer_idx);
        return;
    }

    uint8_t peer_reserved[8];
    if (peer_check_handshake(p->hs_buf, mt->torrent->info_hash,
                             p->peer_id, peer_reserved) < 0) {
        peer_close(mt, peer_idx);
        return;
    }

    /* Ya es un peer util: se pasa a modo bloqueante con timeout, que es como se
     * leen despues los mensajes (siempre con select por delante). */
    net_tcp_set_blocking(p->sock);
    net_set_timeout(p->sock, NET_PEER_IO_TIMEOUT_SEC);

    p->supports_ext = (peer_reserved[5] & 0x10) != 0;
    p->state = PEER_STATE_READY;
    p->connected_since = now;
    p->last_bytes = p->downloaded;
    mt->active_peers++;

    peer_send_interested(p->sock);

    /* BEP-10: el handshake extendido se manda SOLO a quien lo ha anunciado en
     * los reservados. A un peer sin el bit extendido, un mensaje de extension
     * (id 20) le parece una violacion del protocolo y cierra la conexion. */
    if (p->supports_ext)
        peer_send_extended_handshake(p->sock);
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
    params.left = (int64_t)((mt->selected_size > 0 ? mt->selected_size
                                                   : mt->total_size) -
                            (int64_t)mt->downloaded);
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
        if (tr->complete > 0 || tr->incomplete > 0) {
            mt->seeders = tr->complete;
            mt->leechers = tr->incomplete;
        }
        peers_reported += tr->num_peers;

        /* Los peers del tracker van al pool: el de Ubuntu solo manda UNO por
         * vuelta, asi que hay que juntarlos con los que traiga PEX. */
        if (tr->num_peers > 0 && tr->peers) {
            for (int i = 0; i < tr->num_peers; i++)
                peer_pool_add(mt, &tr->peers[i]);

            int failed = 0;
            int made = connect_from_pool(mt, MAX_PEER_CONNECTS_PER_ANNOUNCE,
                                         &failed);
            connects_this_round += made;
            connects_ok += made;
            connects_failed += failed;
        }
        tracker_response_free(tr);
    }

    if (mt->active_peers > 0 || count_inflight(mt) > 0) {
        mt->error_type = TERR_NONE;
        mt->error_msg[0] = '\0';
        mt->error_code[0] = '\0';
        ui_log("announce ok: %d tracker(s), %d peer(s) offered, %d started "
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

        if (fresh >= 8)
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

    /* Timeout de verdad (nunca 0: en FreeBSD 0 es "sin limite" y una lectura a
     * medias cuelga el hilo principal, que es el del panel web). */
    ret = peer_recv_message(p->sock, &msg, block_buf, sizeof(block_buf),
                            NET_PEER_MSG_TIMEOUT_SEC);

    if (ret < 0) {
        int stalled = (errno == EAGAIN || errno == EWOULDBLOCK);

        ui_log("peer %d %s (last msg id %u, errno %d '%s')", peer_idx,
               stalled ? "stalled mid-message" : "disconnected",
               (unsigned)msg.msg_id, errno, strerror(errno));
        peer_close(mt, peer_idx);
        return;
    }

    switch (msg.msg_id) {
    case PEER_MSG_CHOKE:
        p->choked = 1;
        return;

    case PEER_MSG_UNCHOKE:
        /* Solo se traza el cambio: hay clientes que mandan unchoke en cada
         * vuelta y llenaban el log con la misma linea. */
        if (p->choked) {
            ui_log("peer %d unchoked us%s", peer_idx,
                   p->has_bitfield ? "" : " (bitfield pending)");
        }
        p->choked = 0;
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

    /* Bloque de datos: se copia en su offset en la ranura de esa pieza. */
    if (ret == 0 && msg.data_len > 0) {
        int slot = rx_slot_for_piece(mt, (int)msg.piece_index);
        inflight_t *fl;

        if (p->sock < 0) return;

        /* Ninguna ranura espera esa pieza (peticion antigua, o la soltamos
         * porque el peer se cayo): se descarta. */
        if (slot < 0)
            return;

        fl = &mt->rx[slot];

        if (!fl->buf || (uint64_t)msg.piece_begin + msg.data_len > fl->length)
            return;
        if (fl->pending > 0)
            fl->pending--;

        memcpy(fl->buf + msg.piece_begin, block_buf, msg.data_len);
        fl->have += msg.data_len;
        fl->last_activity = get_time_sec();
        p->downloaded += msg.data_len;
        mt->bytes_in += msg.data_len;
        mt->rx_blocks++;

        if (mt->rx_blocks <= 3 || mt->rx_blocks % 200 == 0) {
            ui_log("block %zu: piece %u begin %u len %u (%u/%u of piece)",
                   mt->rx_blocks, msg.piece_index, msg.piece_begin,
                   msg.data_len, fl->have, fl->length);
        }

        if (fl->have < fl->length)
            return;  /* Pieza incompleta: faltan bloques */

        /* Pieza completa: verificar SHA-1 y escribir de una vez */
        if (piece_mgr_complete(&mt->piece_mgr, (size_t)fl->piece,
                               fl->buf, fl->length) == 0) {
            file_writer_write_piece(&mt->file_writer, fl->buf,
                                    fl->length, (size_t)fl->piece);
            mt->downloaded += fl->length;
            mt->rx_pieces_done++;
            if (mt->rx_pieces_done % 20 == 0 || mt->rx_pieces_done == 1) {
                ui_log("piece %zu/%zu ok (%.1f MB downloaded)",
                       mt->rx_pieces_done, mt->torrent->num_pieces,
                       (double)mt->downloaded / (1024.0 * 1024.0));
            }
        } else {
            ui_log("piece %d failed SHA-1; retrying", fl->piece);
            piece_mgr_failed(&mt->piece_mgr, (size_t)fl->piece);
        }
        rx_slot_reset(mt, slot);
    }
}

/**
 * Pide bloques por adelantado.
 *
 * El protocolo no admite bloques mayores de 16 KB, asi que cada pieza se pide en
 * varios bloques hasta llenar la ventana (PEER_MAX_REQUESTS) y se va pidiendo mas
 * segun llegan. Se mantienen varias piezas a la vez, una por peer: es lo que
 * permite tirar de todos los que respondan en vez de ir a la velocidad del
 * unico que hubiera.
 */
static void peer_pump(managed_torrent_t *mt)
{
    if (mt->state != TORRENT_DOWNLOADING)
        return;

    /* Ranuras cuyo peer ya no sirve: la pieza vuelve a la cola */
    for (int s = 0; s < mt->rx_slots; s++) {
        torrent_peer_t *serving;

        if (mt->rx[s].piece < 0)
            continue;

        serving = mt->rx[s].peer >= 0 ? &mt->peers[mt->rx[s].peer] : NULL;
        if (!serving || serving->state != PEER_STATE_READY || serving->choked) {
            rx_slot_drop(mt, s);
            continue;
        }

        /* Mudo durante dos minutos: se suelta la ranura para que la coja otro,
         * en vez de quedarse esperando a un peer que no va a contestar. */
        if (get_time_sec() - mt->rx[s].last_activity > 120)
            rx_slot_drop(mt, s);
    }

    /* Asignar una pieza libre a cada ranura que este vacia, con un peer
     * distinto en cada una. Solo se pide a un peer que ha anunciado lo que
     * tiene: pedirle una pieza que no posee es una violacion del protocolo y
     * los clientes desconectan por ello. */
    for (int s = 0; s < mt->rx_slots; s++) {
        int candidate = -1;
        int candidate_piece = -1;

        if (mt->rx[s].piece >= 0)
            continue;

        for (int i = 0; i < mt->num_peers; i++) {
            torrent_peer_t *peer = &mt->peers[i];
            int busy = 0;
            int piece;

            if (peer->state != PEER_STATE_READY || peer->choked ||
                !peer->has_bitfield)
                continue;

            /* Un peer por pieza, para que la ventana de peticiones de cada uno
             * siga siendo simple y el trabajo se reparta. */
            for (int t = 0; t < mt->rx_slots; t++) {
                if (mt->rx[t].piece >= 0 && mt->rx[t].peer == i) {
                    busy = 1;
                    break;
                }
            }
            if (busy)
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
            break;  /* Ningun peer disponible con piezas que necesitemos */

        /* El buffer se reserva la primera vez que se usa esa ranura. */
        if (!mt->rx[s].buf) {
            mt->rx[s].buf = malloc((size_t)mt->torrent->piece_length);
            if (!mt->rx[s].buf)
                break;
        }

        mt->rx[s].piece = candidate_piece;
        mt->rx[s].length = (uint32_t)mt->torrent->piece_length;
        if ((size_t)candidate_piece == mt->torrent->num_pieces - 1) {
            mt->rx[s].length = (uint32_t)(mt->torrent->total_size -
                              (int64_t)candidate_piece * mt->torrent->piece_length);
        }
        mt->rx[s].have = 0;
        mt->rx[s].next = 0;
        mt->rx[s].pending = 0;
        mt->rx[s].peer = candidate;
        mt->rx[s].last_activity = get_time_sec();
        piece_mgr_request(&mt->piece_mgr, (size_t)candidate_piece);
    }

    /* Llenar la ventana de peticiones de cada ranura */
    for (int s = 0; s < mt->rx_slots; s++) {
        if (mt->rx[s].piece < 0)
            continue;

        while (mt->rx[s].pending < PEER_MAX_REQUESTS &&
               mt->rx[s].next < mt->rx[s].length) {
            uint32_t remaining = mt->rx[s].length - mt->rx[s].next;
            uint32_t len = remaining > PEER_BLOCK_SIZE ? PEER_BLOCK_SIZE : remaining;
            torrent_peer_t *p = &mt->peers[mt->rx[s].peer];

            if (peer_send_request(p->sock, (uint32_t)mt->rx[s].piece,
                                  mt->rx[s].next, len) < 0) {
                rx_slot_drop(mt, s);
                break;
            }

            p->last_request = (uint32_t)mt->rx[s].piece;
            mt->rx[s].next += len;
            mt->rx[s].pending++;
        }
    }
}

/**
 * Presupuesto de tiempo por vuelta para procesar peers, en milisegundos.
 *
 * El motor y el panel web comparten hilo, asi que una vuelta larga es una web
 * que no carga. Lo que no entre aqui se atiende en la vuelta siguiente (10 ms
 * despues): no se pierde nada, solo se reparte.
 */
#define TICK_PEER_BUDGET_MS 120

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

        /* Calculate speed. Se mide con los bytes RECIBIDOS, no con los que ya
         * han completado pieza: si no, la velocidad sale a saltos de 0 KB/s y
         * 128 KB/s aunque se este bajando de forma continua. */
        if (now > mt->last_speed_calc) {
            uint64_t elapsed = now - mt->last_speed_calc;
            if (elapsed >= 2) {
                mt->speed_down = (mt->bytes_in - mt->last_speed_bytes) / elapsed;
                mt->speed_up = (mt->uploaded - mt->last_uploaded) / elapsed;
                mt->last_speed_calc = now;
                mt->last_speed_bytes = mt->bytes_in;
                mt->last_uploaded = mt->uploaded;

                /* Velocidad por peer, para la ficha del panel. */
                for (int p = 0; p < mt->num_peers; p++) {
                    torrent_peer_t *peer = &mt->peers[p];

                    if (peer->downloaded >= peer->last_bytes)
                        peer->speed = (peer->downloaded - peer->last_bytes) / elapsed;
                    else
                        peer->speed = 0;
                    peer->last_bytes = peer->downloaded;
                }
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

        /* Conexiones a medias: avanzar su fase (connect y handshake) sin
         * bloquear. Son las que en un enjambre lleno de NAT se llevan la mayor
         * parte del trabajo y del tiempo. */
        for (int p = 0; p < mt->num_peers; p++) {
            if (mt->peers[p].state == PEER_STATE_CONNECTING ||
                mt->peers[p].state == PEER_STATE_HANDSHAKING)
                peer_step_connecting(mt, p);
        }

        /* Peers listos: drenar todo lo disponible. Un mensaje por vuelta ataba
         * la descarga a la cadencia del bucle y dejaba bloques esperando.
         *
         * Con presupuesto de tiempo: medido en consola, con 26 peers y varios
         * atascados la vuelta tardaba entre 2 y 21 s y dejaba el panel muerto.
         * Lo que no entre aqui se atiende en la vuelta siguiente (10 ms despues). */
        {
            uint64_t phase_start = get_time_ms_local();

            for (int p = 0; p < mt->num_peers; p++) {
                if (mt->peers[p].state != PEER_STATE_READY)
                    continue;

                if (get_time_ms_local() - phase_start > TICK_PEER_BUDGET_MS)
                    break;

                for (int guard = 0; guard < 64; guard++) {
                    struct timeval tv = {0, 0};
                    fd_set readfds;
                    FD_ZERO(&readfds);
                    FD_SET(mt->peers[p].sock, &readfds);

                    if (select(mt->peers[p].sock + 1, &readfds, NULL, NULL, &tv) <= 0)
                        break;

                    process_peer(mt, p);
                    if (mt->peers[p].state != PEER_STATE_READY)
                        break;

                    if (get_time_ms_local() - phase_start > TICK_PEER_BUDGET_MS)
                        break;
                }
            }
        }

        /* Keep-alive: sin esto un peer que no oye nada durante dos minutos nos
         * cierra la conexion. Con un solo peer, perderlo es quedarse parado. */
        for (int p = 0; p < mt->num_peers; p++) {
            torrent_peer_t *peer = &mt->peers[p];

            if (peer->state != PEER_STATE_READY ||
                now - peer->last_keepalive < 120)
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

            if (peer->state != PEER_STATE_READY || peer->ext_id_ut_pex == 0)
                continue;
            if (peer->last_pex_sent != 0 && now - peer->last_pex_sent < 60)
                continue;

            for (int q = 0; q < mt->num_peers && n < 64; q++) {
                if (mt->peers[q].state != PEER_STATE_READY)
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

        /* Candidatos del pool (del tracker, de PEX y de la DHT). Lanzar un
         * connect ya no bloquea, asi que se prueban muchos de golpe: es lo que
         * convierte minutos en segundos cuando 26 de cada 30 estan detras de NAT. */
        if (mt->active_peers < 30 && mt->peer_pool_count > 0 &&
            now - mt->last_pool_connect >= 2) {
            mt->last_pool_connect = now;
            connect_from_pool(mt, 16, NULL);
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
} json_writer_t;static void json_writef(json_writer_t *writer, const char *fmt, ...)
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
            count_live_peers(mt),
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
 * Ficheros del torrent en JSON, para el selector del panel.
 *
 * El buffer se reserva aqui (es del tamaño de los nombres) porque un .torrent
 * puede traer cientos de ficheros con rutas largas.
 */
char *torrent_mgr_files_json(int index, size_t *out_len)
{
    managed_torrent_t *mt;
    json_writer_t writer;
    char *buf;
    size_t cap = 1024;

    if (index < 0 || index >= num_torrents)
        return NULL;

    mt = &torrents[index];
    if (!mt->torrent)
        return NULL;

    for (size_t i = 0; i < mt->torrent->num_files; i++)
        cap += mt->torrent->files[i].path_len * 2 + 128;

    buf = malloc(cap);
    if (!buf)
        return NULL;

    writer.buf = buf;
    writer.size = cap;
    writer.pos = 0;

    json_writef(&writer, "{\"id\":%d,\"name\":", index);
    json_write_string(&writer, mt->name);
    json_writef(&writer,
                ",\"singleFile\":%s,\"totalSize\":%lld,\"selectedSize\":%lld,"
                "\"pieceLength\":%lld,\"files\":[",
                mt->torrent->is_multi_file ? "false" : "true",
                (long long)mt->torrent->total_size,
                (long long)(mt->selected_size > 0 ? mt->selected_size
                                                  : mt->torrent->total_size),
                (long long)mt->torrent->piece_length);

    if (!mt->torrent->is_multi_file) {
        json_writef(&writer, "{\"index\":0,\"path\":");
        json_write_string(&writer, mt->torrent->file_name ? mt->torrent->file_name
                                                          : mt->name);
        json_writef(&writer, ",\"size\":%lld,\"selected\":true}",
                    (long long)mt->torrent->length);
    } else {
        for (size_t i = 0; i < mt->torrent->num_files; i++) {
            int sel = !mt->file_selected || mt->file_selected[i];

            if (i > 0)
                json_writef(&writer, ",");
            json_writef(&writer, "{\"index\":%zu,\"path\":", i);
            json_write_string(&writer, mt->torrent->files[i].path);
            json_writef(&writer, ",\"size\":%lld,\"selected\":%s}",
                        (long long)mt->torrent->files[i].length,
                        sel ? "true" : "false");
        }
    }

    json_writef(&writer, "]}");

    if (out_len)
        *out_len = writer.pos;

    return buf;
}

/** Nombre del cliente a partir del peer_id estilo Azureus ("-qB4650-"). */
static void peer_client_name(const unsigned char id[20], char *out, size_t cap)
{
    static const struct { char a, b; const char *name; } map[] = {
        { 'q', 'B', "qBittorrent" }, { 'T', 'R', "Transmission" },
        { 'l', 't', "libtorrent" },  { 'U', 'T', "uTorrent" },
        { 'D', 'E', "Deluge" },      { 'A', 'Z', "Azureus" },
        { 'B', 'T', "BitTorrent" },  { 'K', 'T', "KTorrent" },
        { 'A', 'T', "Artemis" },     { 'F', 'D', "Free Download Manager" },
        { 'R', 'Q', "rTorrent" },    { 'W', 'W', "WebTorrent" },
        { 'P', 'I', "PicoTorrent" }, { 'H', 'L', "Halite" },
        { 'T', 'S', "TorrentStorm" },{ 'N', 'M', "NiceTorrent" },
    };

    if (cap == 0)
        return;

    if (id[0] == '-' && id[7] == '-') {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if ((char)id[1] == map[i].a && (char)id[2] == map[i].b) {
                snprintf(out, cap, "%s", map[i].name);
                return;
            }
        }
        /* Desconocido: el identificador crudo, que ya dice algo */
        snprintf(out, cap, "%.8s", (const char *)id);
        return;
    }

    snprintf(out, cap, "%s", id[0] ? "?" : "-");
}

/**
 * Peers del torrent en JSON, para la ficha desplegable del panel.
 *
 * Se ordenan por bytes recibidos, que es justo lo que se quiere ver: de quien
 * estas descargando de verdad.
 */
char *torrent_mgr_peers_json(int index, size_t *out_len)
{
    managed_torrent_t *mt;
    json_writer_t writer;
    char *buf;
    size_t cap;
    int order[MAX_PEERS_PER_TORRENT];
    int count = 0;
    uint64_t now = get_time_sec();

    if (index < 0 || index >= num_torrents)
        return NULL;

    mt = &torrents[index];
    if (!mt->torrent)
        return NULL;

    cap = 1024 + (size_t)MAX_PEERS_PER_TORRENT * 360;
    buf = malloc(cap);
    if (!buf)
        return NULL;

    writer.buf = buf;
    writer.size = cap;
    writer.pos = 0;

    /* Solo peers vivos: los huecos libres de la tabla no cuentan. */
    for (int i = 0; i < mt->num_peers; i++) {
        if (mt->peers[i].state != PEER_STATE_IDLE)
            order[count++] = i;
    }

    /* Insercion por bytes recibidos (como mucho MAX_PEERS_PER_TORRENT). */
    for (int i = 1; i < count; i++) {
        int v = order[i];
        int j = i - 1;

        while (j >= 0 && mt->peers[order[j]].downloaded < mt->peers[v].downloaded) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }

    json_writef(&writer, "{\"id\":%d,\"name\":", index);
    json_write_string(&writer, mt->name);
    json_writef(&writer,
                ",\"seeders\":%d,\"leechers\":%d,\"totalPieces\":%zu,\"peers\":[",
                mt->seeders, mt->leechers, mt->torrent->num_pieces);

    for (int k = 0; k < count; k++) {
        torrent_peer_t *p = &mt->peers[order[k]];
        char addr[64];
        char client[40];
        size_t have = 0;
        const char *state = "connecting";

        if (p->has_bitfield && p->bitfield) {
            for (size_t pi = 0; pi < mt->torrent->num_pieces; pi++) {
                if (p->bitfield[pi >> 3] & (1u << (7 - (pi & 7))))
                    have++;
            }
        }

        switch (p->state) {
        case PEER_STATE_CONNECTING:  state = "connecting"; break;
        case PEER_STATE_HANDSHAKING: state = "handshake"; break;
        case PEER_STATE_READY:       state = p->choked ? "choked" : "active"; break;
        default:                     state = "idle"; break;
        }

        snprintf(addr, sizeof(addr), "%u.%u.%u.%u:%u",
                 (ntohl(p->addr.ip) >> 24) & 0xff,
                 (ntohl(p->addr.ip) >> 16) & 0xff,
                 (ntohl(p->addr.ip) >> 8) & 0xff,
                 ntohl(p->addr.ip) & 0xff,
                 ntohs(p->addr.port));
        peer_client_name(p->peer_id, client, sizeof(client));

        if (k > 0)
            json_writef(&writer, ",");

        json_writef(&writer, "{\"addr\":");
        json_write_string(&writer, addr);
        json_writef(&writer, ",\"client\":");
        json_write_string(&writer, client);
        json_writef(&writer,
                    ",\"have\":%zu,\"downloaded\":%llu,\"speed\":%llu,"
                    "\"state\":\"%s\",\"age\":%llu}",
                    have, (unsigned long long)p->downloaded,
                    (unsigned long long)p->speed, state,
                    (unsigned long long)(p->connected_since > 0 &&
                                         now > p->connected_since
                                         ? now - p->connected_since : 0));
    }

    json_writef(&writer, "]}");

    if (out_len)
        *out_len = writer.pos;

    return buf;
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
