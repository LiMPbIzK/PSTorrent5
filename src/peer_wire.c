#include "peer_wire.h"
#include "net_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

void peer_handshake_init(peer_handshake_t *hs,
                         const unsigned char info_hash[20],
                         const unsigned char peer_id[20])
{
    memset(hs, 0, sizeof(peer_handshake_t));
    hs->pstrlen = 19;
    memcpy(hs->pstr, "BitTorrent protocol", 19);
    /* Set reserved bits: BEP-10 (LTEP), BEP-23 (extended) */
    hs->reserved[5] |= 0x10; // Bit 5: extended messaging
    hs->reserved[7] |= 0x01; // Bit 7: BEP-10
    memcpy(hs->info_hash, info_hash, 20);
    memcpy(hs->peer_id, peer_id, 20);
}

int peer_send_handshake(int sock,
                        const unsigned char info_hash[20],
                        const unsigned char peer_id[20])
{
    peer_handshake_t hs;

    peer_handshake_init(&hs, info_hash, peer_id);

    return net_send_all(sock, &hs, sizeof(hs));
}

int peer_check_handshake(const unsigned char *buf,
                         const unsigned char info_hash[20],
                         unsigned char peer_id_out[20],
                         uint8_t reserved_out[8])
{
    const peer_handshake_t *hs = (const peer_handshake_t *)buf;

    if (hs->pstrlen != 19)
        return -1;
    if (memcmp(hs->pstr, "BitTorrent protocol", 19) != 0)
        return -1;
    if (memcmp(hs->info_hash, info_hash, 20) != 0)
        return -1;

    /* Output peer ID if requested */
    if (peer_id_out)
        memcpy(peer_id_out, hs->peer_id, 20);

    /* Reservados del peer: dicen que extensiones soporta (BEP-10 -> byte 5
     * bit 0x10). No se pueden ignorar: hablarle de extensiones a quien no las
     * ha anunciado es una violacion de protocolo y el peer cierra. */
    if (reserved_out)
        memcpy(reserved_out, hs->reserved, 8);

    return 0;
}

int peer_send_interested(int sock)
{
    unsigned char msg[5];
    uint32_t len = 1; // length field
    len = htonl(len);
    memcpy(msg, &len, 4);
    msg[4] = PEER_MSG_INTERESTED;

    return net_send_all(sock, msg, 5);
}

int peer_send_keepalive(int sock)
{
    unsigned char msg[4] = {0, 0, 0, 0};

    return net_send_all(sock, msg, 4);
}

/**
 * Mensaje "extended" (id 20): 4 B de longitud, id, sub-id y el payload.
 */
static int peer_send_extended(int sock, uint8_t ext_id,
                              const unsigned char *payload, size_t len)
{
    unsigned char msg[6 + 1024];
    uint32_t net_len;

    if (len > sizeof(msg) - 6)
        return -1;

    net_len = htonl((uint32_t)(2 + len));
    memcpy(msg, &net_len, 4);
    msg[4] = PEER_MSG_EXTENDED;
    msg[5] = ext_id;
    if (len > 0)
        memcpy(msg + 6, payload, len);

    return net_send_all(sock, msg, 6 + len);
}

int peer_send_extended_handshake(int sock)
{
    /* {"m":{"ut_pex":1},"v":"PSTorrent5"} — las claves van ordenadas, que es
     * lo que exige la forma canonica del bencode.
     *
     * Las longitudes las calcula snprintf a partir de las cadenas: escribirlas
     * a mano es una trampa. Este mensaje estuvo mal formado (9 por 10 en "v" y
     * 8 por 6 en "ut_pex") y el sintoma era que el peer nos cerraba la conexion
     * al instante sin decir nada. */
    static const char version[] = "PSTorrent5";
    static const char pex_key[] = "ut_pex";
    char dict[96];
    int len = snprintf(dict, sizeof(dict), "d1:md%zu:%si%dee1:v%zu:%se",
                       sizeof(pex_key) - 1, pex_key, PEER_EXT_UT_PEX,
                       sizeof(version) - 1, version);

    if (len < 0 || (size_t)len >= sizeof(dict))
        return -1;

    return peer_send_extended(sock, PEER_EXT_HANDSHAKE,
                              (const unsigned char *)dict, (size_t)len);
}

int peer_send_ut_pex(int sock, uint8_t ext_id,
                     const unsigned char *compact, size_t count)
{
    unsigned char payload[1024];
    char head[32];
    int head_len;
    size_t bytes, pos;

    if (count > 64)
        count = 64;
    bytes = count * 6;
    if (bytes > sizeof(payload) - 16)
        return -1;

    /* bencode: d5:added<len>:<bytes>e */
    head_len = snprintf(head, sizeof(head), "d5:added%zu:", bytes);
    if (head_len < 0 || (size_t)head_len >= sizeof(head))
        return -1;

    memcpy(payload, head, (size_t)head_len);
    pos = (size_t)head_len;
    memcpy(payload + pos, compact, bytes);
    pos += bytes;
    payload[pos++] = 'e';

    return peer_send_extended(sock, ext_id, payload, pos);
}

int peer_send_request(int sock, uint32_t index, uint32_t begin, uint32_t length){
    unsigned char msg[17];
    uint32_t len = htonl(13); // 1 (id) + 12 (payload)
    memcpy(msg, &len, 4);
    msg[4] = PEER_MSG_REQUEST;

    uint32_t n_index = htonl(index);
    uint32_t n_begin = htonl(begin);
    uint32_t n_length = htonl(length);

    memcpy(msg + 5, &n_index, 4);
    memcpy(msg + 9, &n_begin, 4);
    memcpy(msg + 13, &n_length, 4);

    return net_send_all(sock, msg, 17);
}

int peer_recv_message(int sock,
                      peer_msg_t *msg,
                      unsigned char *data,
                      size_t data_size,
                      int timeout_sec)
{
    uint8_t msg_id;

    if (msg) {
        msg->msg_id = 0xff;
        msg->piece_index = 0;
        msg->piece_begin = 0;
        msg->data_len = 0;
        msg->bitfield_len = 0;
    }

    if (net_set_timeout(sock, timeout_sec) < 0)
        return -1;

    /* Read message length (4 bytes) */
    uint32_t net_len;
    if (net_recv_exact(sock, &net_len, 4) < 0)
        return -1;

    uint32_t msg_len = ntohl(net_len);

    if (msg_len == 0) {
        /* Keep-alive */
        return 1;
    }

    /* Read message ID */
    if (net_recv_exact(sock, &msg_id, 1) < 0)
        return -1;

    if (msg) msg->msg_id = msg_id;

    switch (msg_id) {
    case PEER_MSG_CHOKE:
        return 1; // We're choked

    case PEER_MSG_UNCHOKE:
        return 1; // We're unchoked

    case PEER_MSG_INTERESTED:
        return 1;

    case PEER_MSG_NOT_INTERESTED:
        return 1;

    case PEER_MSG_HAVE: {
        /* Read piece index (4 bytes) */
        uint32_t net_idx;
        if (net_recv_exact(sock, &net_idx, 4) < 0) return -1;
        if (msg) msg->piece_index = ntohl(net_idx);
        return 1;
    }

    case PEER_MSG_BITFIELD: {
        /* Las piezas que tiene el peer: se copian al buffer del llamante para
         * no pedirle nunca algo que no tiene (los clientes desconectan al
         * peer que lo hace). */
        uint32_t payload_len = msg_len - 1;

        if (msg && msg->bitfield && payload_len > 0) {
            uint32_t keep = payload_len;
            if (keep > msg->bitfield_size)
                keep = (uint32_t)msg->bitfield_size;

            if (net_recv_exact(sock, msg->bitfield, keep) < 0) return -1;
            msg->bitfield_len = (int)keep;

            uint32_t rest = payload_len - keep;
            if (rest > 0) {
                unsigned char *skip = malloc(rest);
                if (skip) { net_recv_exact(sock, skip, rest); free(skip); }
            }
            return 1;
        }

        if (payload_len > 0) {
            unsigned char *skip = malloc(payload_len);
            if (!skip) return -1;
            net_recv_exact(sock, skip, payload_len);
            free(skip);
        }
        return 1;
    }

    case PEER_MSG_PIECE: {
        /* Read piece: index (4) + begin (4) + block data */
        if (msg_len < 9) return -1;

        uint32_t net_idx, net_begin;
        if (net_recv_exact(sock, &net_idx, 4) < 0) return -1;
        if (net_recv_exact(sock, &net_begin, 4) < 0) return -1;

        uint32_t block_len = msg_len - 9;

        if (data && block_len > 0) {
            /* Un peer puede mandar bloques mayores de lo pedido: se lee solo
             * lo que cabe en el buffer y se descarta el resto, para no
             * desbordarlo. */
            uint32_t keep = block_len;
            if (data_size > 0 && keep > data_size)
                keep = (uint32_t)data_size;

            if (net_recv_exact(sock, data, keep) < 0)
                return -1;

            uint32_t rest = block_len - keep;
            if (rest > 0) {
                unsigned char *skip = malloc(rest);
                if (skip) { net_recv_exact(sock, skip, rest); free(skip); }
                block_len = keep;
            }
        } else {
            unsigned char *skip = malloc(block_len);
            if (!skip) return -1;
            net_recv_exact(sock, skip, block_len);
            free(skip);
            block_len = 0;
        }

        if (msg) {
            msg->piece_index = ntohl(net_idx);
            msg->piece_begin = ntohl(net_begin);
            msg->data_len = block_len;
        }

        return 0; // Piece data received
    }
    case PEER_MSG_CANCEL:
        /* Skip payload */
        if (msg_len > 1) {
            unsigned char *skip = malloc(msg_len - 1);
            if (skip) { net_recv_exact(sock, skip, msg_len - 1); free(skip); }
        }
        return 1;

    case PEER_MSG_EXTENDED: {
        /* sub-id (1 byte) + payload. El handshake extendido y ut_pex viajan
         * aqui: se copian al buffer del llamante para poder interpretarlos. */
        if (msg_len < 2) return -1;

        uint8_t ext_id;
        if (net_recv_exact(sock, &ext_id, 1) < 0) return -1;

        uint32_t payload_len = msg_len - 2;

        if (msg) {
            msg->ext_id = ext_id;
            msg->data_len = 0;
        }

        if (data && payload_len > 0) {
            uint32_t keep = payload_len;
            if (data_size > 0 && keep > data_size)
                keep = (uint32_t)data_size;

            if (net_recv_exact(sock, data, keep) < 0) return -1;

            uint32_t rest = payload_len - keep;
            if (rest > 0) {
                unsigned char *skip = malloc(rest);
                if (skip) { net_recv_exact(sock, skip, rest); free(skip); }
            }
            if (msg) msg->data_len = keep;
            return 1;
        }

        if (payload_len > 0) {
            unsigned char *skip = malloc(payload_len);
            if (!skip) return -1;
            net_recv_exact(sock, skip, payload_len);
            free(skip);
        }
        return 1;
    }

    default:
        /* Unknown message - skip payload */
        if (msg_len > 1) {
            unsigned char *skip = malloc(msg_len - 1);
            if (skip) { net_recv_exact(sock, skip, msg_len - 1); free(skip); }
        }
        return 1;
    }
}
