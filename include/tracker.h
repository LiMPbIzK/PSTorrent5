#ifndef PS5TORRENT_TRACKER_H
#define PS5TORRENT_TRACKER_H

#include <stdint.h>
#include <stddef.h>
#include "sha1.h"

/**
 * Maximum number of peers from a tracker response.
 */
#define MAX_TRACKER_PEERS 200

/**
 * A peer address (compact representation).
 */
typedef struct {
    uint32_t ip;        // IPv4 address (network byte order)
    uint16_t port;      // Port (network byte order)
} peer_addr_t;

/**
 * Tracker response.
 */
typedef struct {
    peer_addr_t *peers;
    int          num_peers;
    int          interval;     // Seconds to wait between announces
    int          complete;     // Number of seeders
    int          incomplete;   // Number of leechers
    char        *failure_reason; // If non-NULL, tracker returned error
} tracker_response_t;

/**
 * Params for tracker announce request.
 */
typedef struct {
    const unsigned char *info_hash;     // 20-byte info hash
    const unsigned char *peer_id;       // 20-byte peer ID
    uint16_t             port;          // Listening port (we don't listen, but protocol requires it)
    int64_t              uploaded;      // Bytes uploaded
    int64_t              downloaded;    // Bytes downloaded
    int64_t              left;          // Bytes left to download
    int                  compact;       // 1 = compact response
} tracker_params_t;

/**
 * Announce to an HTTP tracker.
 * @param tracker_url  The announce URL (e.g. "http://tracker.example.com/announce")
 * @param params       Announce parameters
 * @return Tracker response (free with tracker_response_free()), or NULL on error
 */
tracker_response_t *tracker_announce(const char *tracker_url,
                                     const tracker_params_t *params);

/**
 * Trackers publicos que se anaden a los de cada .torrent.
 *
 * Medido el 16-10-2026: los trackers del .torrent pueden estar filtrados por la
 * red o repartir un solo peer; los UDP de esta lista responden con decenas de
 * peers para el mismo torrent. Para desactivarlos, vaciar la lista.
 *
 * @param count Salida: cuantos hay
 * @return Lista de URLs (no liberar)
 */
const char *const *tracker_extra_list(size_t *count);

/**
 * Free a tracker response.
 */
void tracker_response_free(tracker_response_t *resp);

/**
 * Generate a random 20-byte peer ID (Azureus-style).
 * @param peer_id  Output buffer (must be 20 bytes)
 */
void tracker_generate_peer_id(unsigned char peer_id[20]);

#endif /* PS5TORRENT_TRACKER_H */
