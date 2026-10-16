#ifndef PSTORRENT5_NET_HTTP_H
#define PSTORRENT5_NET_HTTP_H

#include <stddef.h>

/**
 * Cliente HTTP de la consola (libSceHttp2 + libSceSsl).
 *
 * Existe por dos motivos:
 *   1. Los trackers modernos usan **HTTPS** (`https://torrent.ubuntu.com/...`)
 *      y el cliente propio solo habla HTTP en claro.
 *   2. Evita portar una libreria TLS: la consola ya trae la suya, con
 *      validacion de certificados, gzip y redirecciones.
 *
 * Si la inicializacion falla (por ejemplo si el servicio no esta disponible en
 * el entorno), el motor sigue funcionando con el cliente propio para HTTP.
 */

/**
 * Inicializa la pila HTTP/SSL de la consola.
 * @param user_agent Cadena que se envia como User-Agent
 * @return 0 si quedo disponible, -1 si no (se usara solo el cliente propio)
 */
int net_http_init(const char *user_agent);

/**
 * @return 1 si la pila HTTP de la consola esta lista, 0 si no
 */
int net_http_available(void);

/**
 * GET de una URL http:// o https://.
 *
 * @param url       URL completa, con la query incluida
 * @param out       Buffer de salida (recibe el cuerpo, sin cabeceras)
 * @param out_size  Tamano del buffer
 * @param status    Codigo HTTP de respuesta (puede ser NULL)
 * @return Bytes de cuerpo leidos, o -1 si no se pudo completar la peticion
 */
int net_http_get(const char *url, unsigned char *out, size_t out_size,
                 int *status);

/**
 * Prueba la pila contra dos URLs conocidas (una HTTP y otra HTTPS) y lo deja
 * en el log. Sirve para saber si un fallo de tracker es de la pila o de la URL.
 */
void net_http_selftest(void);

/**
 * Libera los recursos de la pila HTTP.
 */
void net_http_shutdown(void);

#endif /* PSTORRENT5_NET_HTTP_H */
