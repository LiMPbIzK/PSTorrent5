#ifndef PS5TORRENT_PEER_WIRE_H
#define PS5TORRENT_PEER_WIRE_H

#include <stdint.h>
#include <stddef.h>
#include "sha1.h"
#include "tracker.h"

/**
 * Peer wire protocol constants.
 */
#define PEER_HANDSHAKE_LEN  68
#define PEER_BLOCK_SIZE     (16 * 1024)  // 16KB blocks
#define PEER_MAX_REQUESTS   16            // Pipelined requests
#define PEER_MSG_KEEPALIVE  -1
#define PEER_MSG_CHOKE      0
#define PEER_MSG_UNCHOKE    1
#define PEER_MSG_INTERESTED 2
#define PEER_MSG_NOT_INTERESTED 3
#define PEER_MSG_HAVE       4
#define PEER_MSG_BITFIELD   5
#define PEER_MSG_REQUEST    6
#define PEER_MSG_PIECE      7
#define PEER_MSG_CANCEL     8
#define PEER_MSG_EXTENDED   20

/**
 * Extension protocol (BEP-10) / ut_pex (BEP-11).
 *
 * El sub-id 0 es siempre el handshake extendido, donde cada lado dice que
 * extensiones tiene y con que numero quiere que se le hablen.
 */
#define PEER_EXT_HANDSHAKE  0
#define PEER_EXT_UT_PEX     1   // Nuestro numero para ut_pex

/**
 * A peer wire message header.
 */
typedef struct {
    uint32_t length;    // Message length (excluding this field)
    uint8_t  id;        // Message ID
} __attribute__((packed)) peer_msg_header_t;

/**
 * Handshake structure.
 */
typedef struct {
    uint8_t  pstrlen;                           // Always 19
    uint8_t  pstr[19];                          // "BitTorrent protocol"
    uint8_t  reserved[8];                       // Reserved bits
    uint8_t  info_hash[SHA1_DIGEST_SIZE];       // 20-byte info hash
    uint8_t  peer_id[20];                       // 20-byte peer ID
} __attribute__((packed)) peer_handshake_t;

/**
 * Initialize a handshake message.
 */
void peer_handshake_init(peer_handshake_t *hs,
                         const unsigned char info_hash[20],
                         const unsigned char peer_id[20]);

/**
 * Perform a BitTorrent handshake with a peer.
 * @param sock      Connected socket
 * @param info_hash 20-byte info hash
 * @param peer_id   20-byte local peer ID
 * @param peer_id_out Output buffer for remote peer's ID (20 bytes), or NULL
 * @return 0 on success, -1 on error
 */
int peer_handshake(int sock,
                   const unsigned char info_hash[20],
                   const unsigned char peer_id[20],
                   unsigned char peer_id_out[20]);

/**
 * Send an "interested" message.
 */
int peer_send_interested(int sock);

/**
 * Send a keep-alive (zero-length message).
 *
 * Un peer que no recibe nada durante dos o tres minutos nos cierra la
 * conexion, y con enjambres pequenos perder al unico peer que tenemos es
 * quedarse sin descarga. Es obligatorio en el protocolo.
 */
int peer_send_keepalive(int sock);

/**
 * Send the extension handshake (BEP-10, sub-id 0).
 * Sin esto el peer no sabe que hablamos ut_pex y no nos manda ningun peer.
 */
int peer_send_extended_handshake(int sock);

/**
 * Send a ut_pex message (BEP-11) with a compact IPv4 peer list (6 bytes each).
 * @param ext_id  Numero que el peer pidio para ut_pex en su handshake
 * @param compact Lista compacta de peers (ip 4 B + puerto 2 B, orden de red)
 * @param count   Numero de peers de esa lista (maximo 64)
 */
int peer_send_ut_pex(int sock, uint8_t ext_id,
                     const unsigned char *compact, size_t count);

/**
 * Send a "request" message for a block.
 */
int peer_send_request(int sock, uint32_t index, uint32_t begin, uint32_t length);

/**
 * A message received from a peer.
 */
typedef struct {
    uint8_t        msg_id;         // PEER_MSG_* recibido
    uint8_t        ext_id;         // Sub-id si msg_id == PEER_MSG_EXTENDED
    uint32_t       piece_index;    // Para HAVE y PIECE
    uint32_t       piece_begin;    // Para PIECE
    uint32_t       data_len;       // Bytes de datos en data
    unsigned char *bitfield;       // Buffer donde copiar un BITFIELD (opcional)
    size_t         bitfield_size;  // Tamano de ese buffer
    int            bitfield_len;   // Bytes realmente copiados
} peer_msg_t;

/**
 * Read and process incoming messages from a peer.
 * @param sock          Connected socket
 * @param msg           Output: mensaje recibido y sus datos
 * @param data          Output buffer for piece data
 * @param data_size     Size of the output buffer (blocks larger than this are
 *                      truncated, never overflowed)
 * @param timeout_sec   Receive timeout
 * @return 0 for piece data, 1 for keepalive/choke/unchoke/have, -1 for error/disconnect
 */
int peer_recv_message(int sock,
                      peer_msg_t *msg,
                      unsigned char *data,
                      size_t data_size,
                      int timeout_sec);

#endif /* PS5TORRENT_PEER_WIRE_H */
