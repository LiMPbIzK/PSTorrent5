#ifndef PS5TORRENT_NET_UTILS_H
#define PS5TORRENT_NET_UTILS_H

#include <stdint.h>
#include <stddef.h>

/**
 * Maximum buffer sizes.
 */
#define NET_BUF_SIZE    65536   // 64KB recv buffer
#define HTTP_BUF_SIZE   8192    // HTTP response buffer

/**
 * Socket timeouts, in seconds.
 *
 * The payload is single-threaded: the BitTorrent engine and the web panel share
 * one thread, so a blocking socket call freezes the whole UI. These bounds are
 * deliberately short; an unreachable peer must never stall the main loop.
 */
#define NET_TRACKER_CONNECT_TIMEOUT_SEC 5
#define NET_PEER_CONNECT_TIMEOUT_SEC    2

/**
 * Espera del handshake de un peer, en segundos.
 *
 * Muchos peers aceptan TCP y luego no dicen nada (NAT, cortafuegos, cliente
 * dormido). Medido sobre una muestra de 30: 4 aceptan TCP y solo 1 completa el
 * handshake. Con 8 s por cada uno, el panel se congelaba de sobra; 4 s es de
 * sobra para un handshake legitimo (que cabe en un solo paquete).
 */
#define NET_PEER_HANDSHAKE_TIMEOUT_SEC  4

#define NET_TRACKER_IO_TIMEOUT_SEC      8
#define NET_PEER_IO_TIMEOUT_SEC         8

/**
 * Espera maxima para completar UN mensaje de un peer, en segundos.
 *
 * Critico: con 0, `SO_RCVTIMEO` queda a cero, que en FreeBSD y Linux significa
 * **sin timeout**, y una lectura a medias de un mensaje se queda colgada para
 * siempre. Eso congela el hilo entero (el panel web incluido). Medido en consola:
 * con 40 peers y trafico de PEX el payload se quedo clavado y hubo que relanzarlo.
 *
 * Con 2 s, un mensaje legitimo (el bloque mas grande son 16 KB) llega de sobra y
 * un peer atascado cuesta como mucho 2 s antes de cerrarlo.
 */
#define NET_PEER_MSG_TIMEOUT_SEC        2

/**
 * Espera de un tracker UDP (BEP-15), por peticion: hay dos (connect y announce).
 *
 * Los publicos contestan en decimas de segundo, asi que 3 s es de sobra y
 * mantiene acotada la pausa del panel web, que comparte este hilo.
 */
#define NET_UDP_TRACKER_TIMEOUT_SEC     3

/**
 * Initialize networking subsystem.
 * @return 0 on success, -1 on error
 */
int net_init(void);

/**
 * Shutdown networking subsystem.
 */
void net_cleanup(void);

/**
 * Resolve a hostname to an IP address (IPv4).
 * @param hostname  Hostname or IP string
 * @param addr_out  Output: 4-byte IPv4 address (network byte order)
 * @return 0 on success, -1 on error
 */
int net_resolve(const char *hostname, uint32_t *addr_out);

/**
 * Create a TCP socket and connect to a remote host.
 * @param addr  Remote IP address (network byte order)
 * @param port  Remote port (host byte order)
 * @return Socket fd on success, -1 on error
 */
int net_tcp_connect(uint32_t addr, uint16_t port);

/**
 * Create a TCP socket and connect, giving up after timeout_sec seconds.
 *
 * Uses a non-blocking connect plus select() so an unreachable peer cannot
 * freeze the payload: the BitTorrent engine runs on the same thread as the
 * web server. The socket is left in blocking mode on success.
 *
 * @param addr        Remote IP address (network byte order)
 * @param port        Remote port (host byte order)
 * @param timeout_sec Connect timeout in seconds
 * @return Socket fd on success, -1 on error or timeout
 */
int net_tcp_connect_timeout(uint32_t addr, uint16_t port, int timeout_sec);

/**
 * Start a non-blocking connect. The socket stays non-blocking.
 * @return Socket fd (connecting) on success, -1 on error
 */
int net_tcp_connect_start(uint32_t addr, uint16_t port);

/**
 * Check a pending non-blocking connect.
 * @return 1 connected, 0 still trying, -1 failed
 */
int net_tcp_connect_poll(int sock);

/**
 * Put a socket back into blocking mode (peer messages read with timeouts).
 * @return 0 on success, -1 on error
 */
int net_tcp_set_blocking(int sock);

/**
 * Send all data (handles partial sends).
 * @param sock  Socket fd
 * @param data  Data to send
 * @param len   Length of data
 * @return 0 on success, -1 on error
 */
int net_send_all(int sock, const void *data, size_t len);

/**
 * Receive exactly len bytes (blocking).
 * @param sock  Socket fd
 * @param buf   Receive buffer
 * @param len   Number of bytes to receive
 * @return 0 on success, -1 on error/timeout
 */
int net_recv_exact(int sock, void *buf, size_t len);

/**
 * Receive up to len bytes.
 * @param sock  Socket fd
 * @param buf   Receive buffer
 * @param len   Maximum bytes to receive
 * @return Number of bytes received, 0 on close, -1 on error
 */
int net_recv_some(int sock, void *buf, size_t len);

/**
 * Set socket timeout.
 * @param sock        Socket fd
 * @param timeout_sec Timeout in seconds
 * @return 0 on success, -1 on error
 */
int net_set_timeout(int sock, int timeout_sec);

/**
 * Close a socket.
 */
void net_close(int sock);

/**
 * URL encode a binary buffer.
 * @param input     Input data
 * @param input_len Input length
 * @param output    Output buffer (must be at least input_len*3 + 1)
 */
void net_url_encode(const unsigned char *input, size_t input_len, char *output);

/**
 * Create a UDP socket on a local port.
 * @param local_port Local port (0 = efimero)
 * @return Socket fd on success, -1 on error
 */
int net_udp_open(uint16_t local_port);

/**
 * Send one datagram.
 * @param addr Remote IPv4 address (network byte order)
 * @param port Remote port (host byte order)
 * @return 0 on success, -1 on error
 */
int net_udp_send_to(int sock, uint32_t addr, uint16_t port,
                    const void *data, size_t len);

/**
 * Receive one datagram without blocking.
 *
 * Usa select() con tiempo cero, igual que la E/S de peers: el motor comparte
 * hilo con el panel web y no puede quedarse esperando.
 *
 * @param from_addr/from_port Salida con el origen (pueden ser NULL)
 * @return Bytes recibidos, 0 si no habia nada, -1 en error
 */
int net_udp_recv_from(int sock, void *buf, size_t len,
                      uint32_t *from_addr, uint16_t *from_port);

/**
 * Receive one datagram waiting up to timeout_sec.
 * @return Bytes recibidos, 0 si se agoto el tiempo, -1 en error
 */
int net_udp_recv_from_timeout(int sock, void *buf, size_t len,
                              uint32_t *from_addr, uint16_t *from_port,
                              int timeout_sec);

#endif /* PS5TORRENT_NET_UTILS_H */
