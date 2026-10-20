#ifndef PS5TORRENT_TORRENT_MGR_H
#define PS5TORRENT_TORRENT_MGR_H

#include <stdint.h>
#include <stddef.h>
#include "torrent.h"
#include "piece_mgr.h"
#include "file_io.h"
#include "tracker.h"
#include "peer_wire.h"

/**
 * Maximum concurrent torrents.
 */
#define MAX_TORRENTS 8

/**
 * Maximum peers per torrent.
 */
#define MAX_PEERS_PER_TORRENT 32

/**
 * Maximum trackers whose health is remembered per torrent.
 */
#define MAX_TRACKER_STATE 32

/**
 * Maximum trackers tried per announce round.
 *
 * Each tracker that does not answer costs NET_TRACKER_CONNECT_TIMEOUT_SEC and
 * the web panel shares this thread, so the work per round is bounded. Many
 * torrents carry 20+ announce URLs; without this bound the loop would block for
 * minutes and the panel would look dead.
 */
#define MAX_TRACKERS_PER_ANNOUNCE 3

/**
 * Wall-clock budget for one announce round, in seconds.
 *
 * This is the real guarantee that the panel stays usable: the round stops as
 * soon as it is exceeded (it can overshoot by at most one socket timeout).
 */
#define ANNOUNCE_BUDGET_SEC 4

/**
 * Backoff applied to a tracker that did not answer: fails * this value,
 * capped at MAX_TRACKER_BACKOFF_SEC.
 */
#define TRACKER_BACKOFF_STEP_SEC 120
#define MAX_TRACKER_BACKOFF_SEC 600

/**
 * Maximum new peer connections attempted per tracker announce.
 *
 * Each attempt can block for NET_PEER_CONNECT_TIMEOUT_SEC and the web panel
 * shares the engine's thread, so the work per announce is bounded.
 */
#define MAX_PEER_CONNECTS_PER_ANNOUNCE 8

/**
 * Direcciones candidatas guardadas por torrent.
 *
 * Antes los peers solo salian del announce y se conectaban en el acto. Con PEX
 * el peer conectado nos manda decenas de direcciones de golpe, asi que hace
 * falta un sitio donde dejarlas e irlas probando de poco en poco.
 */
#define MAX_PEER_POOL 256

/** Cuanto se espera antes de reintentar una direccion que fallo, en segundos. */
#define PEER_POOL_RETRY_SEC 300

/**
 * Peers intentando conectar a la vez, como maximo.
 *
 * Cada intento es un socket y un hueco, y la mayoria muere (NAT). Tener muchos
 * en vuelo es justo lo que hace que los pocos que responden aparezcan en
 * segundos en vez de en minutos.
 */
#define MAX_PEERS_INFLIGHT 32

/**
 * Una pieza en curso.
 *
 * Los bloques se piden de PEER_BLOCK_SIZE en PEER_BLOCK_SIZE (el protocolo no
 * admite bloques mayores) y se acumulan en `buf` hasta completar la pieza; solo
 * entonces se verifica el SHA-1 y se escribe. Cada ranura la sirve un peer
 * distinto, que es de donde sale el paralelismo.
 */
#define PEER_INFLIGHT_MAX 16

typedef struct {
    unsigned char *buf;           // Buffer de la pieza (se reserva al usarla)
    int            piece;         // Indice de la pieza (-1 = ranura libre)
    uint32_t       length;        // Tamano real de esa pieza, en bytes
    uint32_t       have;          // Bytes acumulados
    uint32_t       next;          // Siguiente offset a pedir
    int            pending;       // Bloques pedidos y aun no recibidos
    int            peer;          // Peer que la sirve (-1 = ninguno)
    uint64_t       last_activity; // Ultimo bloque recibido, para detectar atasco
} inflight_t;

/**
 * Torrent state.
 */
typedef enum {
    TORRENT_STOPPED,
    TORRENT_DOWNLOADING,
    TORRENT_SEEDING,
    TORRENT_ERROR,
    TORRENT_DONE
} torrent_state_t;

/**
 * Torrent error types.
 */
typedef enum {
    TERR_NONE,
    TERR_PARSE_FAILED,
    TERR_TRACKER_FAILED,
    TERR_NO_PEERS,
    TERR_WRITE_FAILED,
    TERR_HASH_MISMATCH
} torrent_error_t;

/**
 * Estado de la conexion con un peer.
 *
 * connect() y el handshake van sin bloquear y por pasos, porque la muestra real
 * medida dice que de 30 candidatos solo 4 aceptan TCP y 1 completa el handshake:
 * probarlos en serie cuesta minutos y ademas congela el panel, que comparte hilo.
 */
typedef enum {
    PEER_STATE_IDLE = 0,       // Hueco libre
    PEER_STATE_CONNECTING,     // connect() lanzado, esperando
    PEER_STATE_HANDSHAKING,    // conectado, nuestro handshake enviado
    PEER_STATE_READY           // handshake hecho: se le pueden pedir piezas
} peer_state_t;

/**
 * A peer connection.
 */
typedef struct {
    int       sock;
    peer_state_t state;
    uint64_t  deadline;        // Limite de la fase actual (connect o handshake)
    unsigned char hs_buf[68];  // Handshake del peer, que llega por trozos
    int       hs_received;
    peer_addr_t addr;
    uint8_t   peer_id[20];
    int       choked;
    int       am_interested;
    int       active;
    uint32_t  last_request;
    uint64_t  downloaded;
    uint64_t  uploaded;
    uint64_t  last_keepalive;

    /* Para la ficha del torrent en el panel: desde cuando esta conectado, lo que
     * nos daba en la ultima muestra y su velocidad. */
    uint64_t  connected_since;
    uint64_t  last_bytes;
    uint64_t  speed;
    unsigned  messages_seen;   // Para limitar las trazas de have/bitfield

    /* Piezas que tiene el peer. Sin esto se le pediria una que no tiene y los
     * clientes desconectan al peer que lo hace (era la causa de que el peer se
     * cayese justo despues del unchoke). */
    unsigned char *bitfield;
    size_t         bitfield_len;
    int            has_bitfield;

    /* BEP-10/11: numero con el que el peer quiere que le hablemos de ut_pex
     * (0 = no lo soporta) y cuando le mandamos la ultima lista. */
    uint8_t        ext_id_ut_pex;
    uint8_t        supports_ext;
    uint64_t       last_pex_sent;
} torrent_peer_t;

/**
 * A managed torrent download.
 */
typedef struct {
    char            id[64];          // Unique ID (hash hex)
    char            name[256];       // Display name
    char            save_path[512];  // Download directory
    unsigned char   info_hash[20];

    torrent_state_t  state;
    torrent_error_t  error_type;
    char             error_msg[256];
    char             error_code[32];  // Stable code for UI translations (see web i18n)

    torrent_t       *torrent;
    piece_mgr_t      piece_mgr;
    file_writer_t    file_writer;

    unsigned char    peer_id[20];
    torrent_peer_t   peers[MAX_PEERS_PER_TORRENT];
    int              num_peers;
    int              active_peers;

    /* Seleccion de ficheros: 1 byte por fichero del .torrent (1 = descargar).
     * NULL = todos. Las piezas que no toca ningun fichero elegido se marcan
     * como saltadas, porque la verificacion SHA-1 es por pieza entera. */
    uint8_t         *file_selected;
    int64_t          selected_size;   // Bytes que suman los ficheros elegidos

    /* Reanudacion: bitfield recuperado del disco (se aplica al arrancar), pieza
     * por la que va la verificacion en segundo plano (-1 = sin verificar) y su
     * buffer de lectura. */
    unsigned char   *restore_pieces;
    int              verify_next;
    unsigned char   *verify_buf;
    uint64_t         last_state_save;

    /* Direcciones candidatas (del tracker y, sobre todo, de ut_pex). */
    struct {
        peer_addr_t addr;
        uint64_t    next_try;
    } peer_pool[MAX_PEER_POOL];
    int              peer_pool_count;
    uint64_t         last_pool_connect;

    // Stats
    uint64_t  downloaded;
    uint64_t  uploaded;
    uint64_t  bytes_in;       // Bytes recibidos (sin verificar), para la velocidad
    uint64_t  total_size;
    int       tracker_announces;
    int       tracker_interval;
    int       seeders;        // Del ultimo announce (complete/incomplete)
    int       leechers;
    float     progress;
    uint64_t  speed_down;     // bytes/sec
    uint64_t  speed_up;

    // Timing
    uint64_t  last_announce_time;
    uint64_t  last_speed_calc;
    uint64_t  last_speed_bytes;
    uint64_t  last_uploaded;
    uint64_t  start_time;

    /* Por que tracker va la siguiente ronda de announce. Sin esto, los dos
     * primeros de la lista que contesten bien acaparan todas las rondas y el
     * resto (incluidos los trackers extra) no se probarian nunca. */
    size_t    announce_cursor;

    // Source info
    int       is_magnet;      // 1 = from magnet link
    char      magnet_uri[1024]; // Original magnet URI
    char      torrent_source[4096]; // Original .torrent data (for magnet)

    /* Per-tracker health. A torrent can list 20+ announce URLs and, if all of
     * them time out, retrying every round would keep the main loop (and the web
     * panel) blocked for minutes. Dead trackers get an increasing backoff. */
    struct {
        uint64_t next_try_sec;  // 0 = may be tried now
        int      fails;
    } tracker_state[MAX_TRACKER_STATE];

    /* Descarga de piezas: varias a la vez, una por peer.
     *
     * Antes era una sola pieza y de un solo peer. Medido el 17-10-2026: en un
     * enjambre donde la mayoria de peers estan detras de NAT, tirar de uno solo
     * es ir a la velocidad del mas lento y quedarse parado cada vez que ese peer
     * no tiene nada que dar. Con varias ranuras se reparten las piezas entre
     * todos los que respondan.
     *
     * El buffer de cada ranura se reserva cuando se usa: su tamano es el de una
     * pieza, que puede ir de 256 KB a 16 MB. */
    inflight_t     rx[PEER_INFLIGHT_MAX];
    int            rx_slots;    // Ranuras en uso, segun el tamano de pieza
    size_t         rx_pieces_done; // Piezas completadas, para trazar progreso
    size_t         rx_blocks;   // Bloques recibidos, para trazar progreso
} managed_torrent_t;

/**
 * Initialize the torrent manager.
 */
void torrent_mgr_init(void);

/**
 * Add a torrent from a parsed torrent file.
 * @param torrent  Parsed torrent (takes ownership)
 * @param save_path Download directory
 * @param is_magnet 1 if from magnet link
 * @param magnet_uri Original magnet URI (NULL if not magnet)
 * @return Torrent index, or -1 on error
 */
int torrent_mgr_add(torrent_t *torrent, const char *save_path,
                    int is_magnet, const char *magnet_uri);

/**
 * Add a torrent from raw .torrent data.
 * @param data        .torrent file content
 * @param data_len    Content length
 * @param save_path   Download directory
 * @return Torrent index, or -1 on error
 */
int torrent_mgr_add_raw(const unsigned char *data, size_t data_len,
                        const char *save_path);

/**
 * Parse a magnet URI and start downloading metadata.
 * @param magnet_uri The magnet:?xt=urn:btih:... URI
 * @param save_path  Download directory
 * @return Torrent index, or -1 on error
 */
int torrent_mgr_add_magnet(const char *magnet_uri, const char *save_path);

/**
 * Start downloading a torrent.
 * @param index Torrent index
 * @return 0 on success, -1 on error
 */
int torrent_mgr_start(int index);

/**
 * Stop a torrent.
 */
void torrent_mgr_stop(int index);

/**
 * Remove a torrent and free resources.
 */
void torrent_mgr_remove(int index);

/**
 * Get a torrent by index.
 */
managed_torrent_t *torrent_mgr_get(int index);

/**
 * Get number of active torrents.
 */
int torrent_mgr_count(void);

/**
 * Get total number of torrent slots (including empty).
 */
int torrent_mgr_capacity(void);

/**
 * Find a torrent by info hash.
 * @return Index, or -1 if not found
 */
int torrent_mgr_find(const unsigned char info_hash[20]);

/**
 * Main tick function - call this periodically to process peers.
 * @return Number of active torrents being processed
 */
int torrent_mgr_tick(void);

/**
 * Recupera los torrents guardados en disco (estado + metainfo) y reanuda los que
 * estaban descargando. Se llama una vez al arrancar, antes del bucle principal.
 */
void torrent_mgr_restore_state(void);

/**
 * Callback que la DHT llama al encontrar un peer (se registra en dht_init).
 */
void torrent_mgr_on_dht_peer(const unsigned char info_hash[20],
                             const peer_addr_t *peer);

/**
 * Fija que ficheros se quieren descargar (1 byte por fichero, 1 = si).
 * Debe llamarse antes de torrent_mgr_start(): la seleccion se traduce a piezas
 * saltadas al arrancar.
 * @param count  Debe coincidir con el numero de ficheros del torrent
 * @return 0 correcto, -1 indice o tamano invalido
 */
int torrent_mgr_set_selection(int index, const unsigned char *selected,
                             size_t count);

/**
 * Ficheros del torrent en JSON, para el selector del panel.
 * @param out_len Salida: longitud del texto
 * @return Buffer reservado (free() al terminar), o NULL
 */
char *torrent_mgr_files_json(int index, size_t *out_len);

/**
 * Peers del torrent en JSON, para la ficha desplegable del panel.
 * @param out_len Salida: longitud del texto
 * @return Buffer reservado (free() al terminar), o NULL
 */
char *torrent_mgr_peers_json(int index, size_t *out_len);

/**
 * Get JSON status string for all torrents.
 * @param buf    Output buffer
 * @param bufsz  Buffer size
 */
void torrent_mgr_status_json(char *buf, size_t bufsz);

/**
 * Get a human-readable error string.
 */
const char *torrent_mgr_error_str(torrent_error_t err);

/**
 * Log a line per torrent with progress, speed and peers. Useful during normal
 * use: it is the quickest way to see whether a download is actually moving.
 */
void torrent_mgr_log_progress(void);

/**
 * Shutdown all torrents and free resources.
 */
void torrent_mgr_shutdown(void);
#endif /* PS5TORRENT_TORRENT_MGR_H */
