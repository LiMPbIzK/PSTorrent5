#ifndef PS5TORRENT_DHT_H
#define PS5TORRENT_DHT_H

#include <stdint.h>
#include <stddef.h>
#include "tracker.h"

/**
 * DHT (BEP-5): tabla de nodos y de info_hash que nos interesan.
 *
 * La tabla es una lista plana, sin k-buckets ni distancia XOR ordenada. Para
 * encontrar peers con unos cientos de nodos es suficiente y ahorra mucha
 * maquinaria; queda anotado como simplificacion consciente.
 */
#define DHT_MAX_NODES    256
#define DHT_MAX_WANTS      4
#define DHT_MAX_DATAGRAM 2048

/**
 * Callback: la DHT ha encontrado un peer para este info_hash.
 *
 * Se llama desde dht_tick(), en el hilo principal.
 */
typedef void (*dht_peer_cb_t)(const unsigned char info_hash[20],
                              const peer_addr_t *peer);

/**
 * Crea el nodo DHT. No bloquea mas alla de la resolucion DNS inicial.
 * @param on_peer Callback para los peers encontrados (obligatorio)
 * @return 0 en exito, -1 si no se pudo abrir el socket
 */
int dht_init(dht_peer_cb_t on_peer);

/** Cierra el socket y olvida todo. */
void dht_shutdown(void);

/** Registra el interes por un info_hash (uno por torrent activo). */
void dht_want(const unsigned char info_hash[20]);

/** Deja de interesarse por un info_hash. */
void dht_unwant(const unsigned char info_hash[20]);

/**
 * Procesa la E/S pendiente y el trabajo periodico. Nunca bloquea: es seguro
 * llamarlo desde el bucle principal que comparte el panel web.
 */
void dht_tick(void);

/** Nodos utiles en la tabla (para el log y el panel). */
int dht_node_count(void);

/** Peers aportados por DHT desde el arranque. */
int dht_peers_found(void);

/** Estado en una linea, para el log. */
void dht_log_status(void);

#endif /* PS5TORRENT_DHT_H */
