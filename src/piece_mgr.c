#include "piece_mgr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void piece_mgr_init(piece_mgr_t *pm, size_t num_pieces,
                    int64_t piece_length, int64_t total_size,
                    const unsigned char *expected_hashes)
{
    memset(pm, 0, sizeof(piece_mgr_t));

    pm->num_pieces = num_pieces;
    pm->piece_length = piece_length;
    pm->total_size = total_size;
    pm->expected_hashes = expected_hashes;

    pm->pieces = calloc(num_pieces, sizeof(piece_t));

    /* Setup bitfield */
    pm->bitfield_len = (num_pieces + 7) / 8;
    pm->bitfield = calloc(pm->bitfield_len, 1);

    /* Initialize pieces */
    for (size_t i = 0; i < num_pieces; i++) {
        pm->pieces[i].index = (uint32_t)i;
        pm->pieces[i].state = PIECE_FREE;
        if (expected_hashes) {
            memcpy(pm->pieces[i].hash, expected_hashes + i * 20, 20);
        }
    }
}

int piece_mgr_get_next(piece_mgr_t *pm,
                       const unsigned char *bitfield,
                       size_t bf_len)
{
    if (!pm || piece_mgr_is_done(pm)) return -1;

    /* Simple strategy: first available piece (random start offset) */
    /* In a full implementation, we would do rarest-first */
    size_t start = 0;

    for (size_t i = 0; i < pm->num_pieces; i++) {
        size_t idx = (start + i) % pm->num_pieces;

        if (pm->pieces[idx].state != PIECE_FREE)
            continue;

        /* Check if peer has this piece */
        if (bitfield && idx / 8 < bf_len) {
            if (!(bitfield[idx / 8] & (1 << (7 - (idx % 8)))))
                continue; // Peer doesn't have this piece
        }

        return (int)idx;
    }

    return -1;
}

void piece_mgr_request(piece_mgr_t *pm, size_t index)
{
    if (!pm || index >= pm->num_pieces) return;
    pm->pieces[index].state = PIECE_REQUESTED;
}

int piece_mgr_complete(piece_mgr_t *pm, size_t index,
                       const unsigned char *data, size_t data_len)
{
    if (!pm || index >= pm->num_pieces) return -1;

    /* Verify SHA-1 hash */
    unsigned char actual_hash[20];
    sha1_hash(data, data_len, actual_hash);

    if (memcmp(actual_hash, pm->pieces[index].hash, 20) != 0) {
        /* Hash mismatch: la pieza vuelve a la cola para reintentarla. Dejarla
         * en PIECE_FAILED la sacaria para siempre de get_next() y la descarga
         * nunca podria completarse. */
        pm->pieces[index].state = PIECE_FREE;
        pm->num_failed++;
        return -1;
    }

    /* Hash matches - piece is good */
    pm->pieces[index].state = PIECE_COMPLETE;
    pm->num_complete++;

    /* Set bit in bitfield */
    if (index / 8 < pm->bitfield_len) {
        pm->bitfield[index / 8] |= (uint8_t)(1 << (7 - (index % 8)));
    }

    return 0;
}

void piece_mgr_failed(piece_mgr_t *pm, size_t index)
{
    if (!pm || index >= pm->num_pieces) return;

    /* Vuelve a PIECE_FREE (no a PIECE_FAILED) porque get_next() solo reparte
     * piezas libres: marcarla como fallida la perderia para siempre. */
    pm->pieces[index].state = PIECE_FREE;
    pm->num_failed++;
}

void piece_mgr_skip(piece_mgr_t *pm, size_t index)
{
    if (!pm || index >= pm->num_pieces)
        return;

    /* Solo tiene sentido sobre una pieza que aun no se ha tocado. */
    if (pm->pieces[index].state != PIECE_FREE)
        return;

    pm->pieces[index].state = PIECE_SKIPPED;
    pm->num_skipped++;
}

void piece_mgr_restore(piece_mgr_t *pm, size_t index)
{
    if (!pm || index >= pm->num_pieces)
        return;

    /* Si ya esta resuelta (completa o saltada por la seleccion), no se toca. */
    if (pm->pieces[index].state != PIECE_FREE)
        return;

    pm->pieces[index].state = PIECE_COMPLETE;
    pm->num_complete++;

    if (index / 8 < pm->bitfield_len)
        pm->bitfield[index / 8] |= (uint8_t)(1 << (7 - (index % 8)));
}

void piece_mgr_unmark(piece_mgr_t *pm, size_t index)
{
    if (!pm || index >= pm->num_pieces)
        return;

    if (pm->pieces[index].state != PIECE_COMPLETE)
        return;

    pm->pieces[index].state = PIECE_FREE;
    if (pm->num_complete > 0)
        pm->num_complete--;

    if (index / 8 < pm->bitfield_len)
        pm->bitfield[index / 8] &= (uint8_t)~(1 << (7 - (index % 8)));
}

void piece_mgr_reset_requested(piece_mgr_t *pm)
{
    if (!pm)
        return;

    for (size_t i = 0; i < pm->num_pieces; i++) {
        if (pm->pieces[i].state == PIECE_REQUESTED)
            pm->pieces[i].state = PIECE_FREE;
    }
}

int piece_mgr_is_done(const piece_mgr_t *pm)
{
    if (!pm) return 1;
    return pm->num_complete + pm->num_skipped >= pm->num_pieces;
}

float piece_mgr_progress(const piece_mgr_t *pm)
{
    size_t wanted;

    if (!pm || pm->num_pieces == 0) return 0.0f;

    /* El progreso va sobre lo que el usuario ha pedido, no sobre el torrent
     * entero: si elige la mitad, la barra llega al 100% al tenerla. */
    wanted = pm->num_pieces - pm->num_skipped;
    if (wanted == 0) return 1.0f;

    return (float)pm->num_complete / (float)wanted;
}

void piece_mgr_destroy(piece_mgr_t *pm)
{
    if (!pm) return;

    free(pm->pieces);
    free(pm->bitfield);
    memset(pm, 0, sizeof(piece_mgr_t));
}
