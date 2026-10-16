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
#define MAX_PEERS_PER_TORRENT 50

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
#define MAX_TRACKERS_PER_ANNOUNCE 2

/**
 * Wall-clock budget for one announce round, in seconds.
 *
 * This is the real guarantee that the panel stays usable: the round stops as
 * soon as it is exceeded (it can overshoot by at most one socket timeout).
 */
#define ANNOUNCE_BUDGET_SEC 8

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
#define MAX_PEER_CONNECTS_PER_ANNOUNCE 2

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
 * A peer connection.
 */
typedef struct {
    int       sock;
    peer_addr_t addr;
    uint8_t   peer_id[20];
    int       choked;
    int       am_interested;
    int       active;
    uint32_t  last_request;
    uint64_t  downloaded;
    uint64_t  uploaded;
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

    // Stats
    uint64_t  downloaded;
    uint64_t  uploaded;
    uint64_t  total_size;
    int       tracker_announces;
    int       tracker_interval;
    float     progress;
    uint64_t  speed_down;     // bytes/sec
    uint64_t  speed_up;

    // Timing
    uint64_t  last_announce_time;
    uint64_t  last_speed_calc;
    uint64_t  last_downloaded;
    uint64_t  last_uploaded;
    uint64_t  start_time;

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

    /* Descarga de piezas, una en curso por torrent.
     *
     * Se pide en bloques de PEER_BLOCK_SIZE (el protocolo no admite bloques
     * mayores) y se van acumulando en rx_buf hasta completar la pieza; solo
     * entonces se verifica el SHA-1 y se escribe en disco. Un torrent puede
     * tener varios peers conectados, pero descarga de uno cada vez: simple,
     * correcto y suficiente para empezar (la E/S no bloqueante del hito 1
     * permitira paralelizar). */
    unsigned char *rx_buf;      // Buffer de la pieza en curso
    int            rx_piece;    // Indice de la pieza en curso (-1 = ninguna)
    uint32_t       rx_length;   // Tamano real de esa pieza, en bytes
    uint32_t       rx_have;     // Bytes acumulados
    uint32_t       rx_next;     // Siguiente offset a pedir
    int            rx_pending;  // Bloques pedidos y aun no recibidos
    int            rx_peer;     // Peer que la esta sirviendo (-1 = ninguno)
    size_t         rx_pieces_done; // Piezas completadas, para trazar progreso
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
 * Shutdown all torrents and free resources.
 */
void torrent_mgr_shutdown(void);

#endif /* PS5TORRENT_TORRENT_MGR_H */
