#ifndef PSTORRENT5_LOG_H
#define PSTORRENT5_LOG_H

#include <stddef.h>

/**
 * Log a fichero del payload.
 *
 * Complementa a klog (que se pierde al reiniciar la consola y necesita un
 * servidor en la red): escribe en un fichero que sobrevive mientras exista la
 * partición, se puede leer por FTP y se expone en `GET /api/log` desde el
 * panel. Todo lo que pasa por ui_log()/ui_error() acaba aquí.
 *
 * Ruta por defecto: /data/pstorrent5/pstorrent5.log, con reserva en /data y
 * /tmp si el directorio no se puede crear o abrir.
 */

#define LOG_PATH_MAX 256

/**
 * Abre el fichero de log (crea el directorio si hace falta).
 * @return 0 si hay fichero de log, -1 si no se pudo abrir ninguno.
 */
int log_init(void);

/**
 * Ruta real del fichero de log, o cadena vacía si no hay ninguno.
 */
const char *log_path(void);

/**
 * Añade una línea con marca de tiempo. Nunca falla de forma visible.
 * @param text Texto ya formateado (una línea, sin salto final)
 */
void log_append(const char *text);

/**
 * Copia el final del log (las últimas ~48 KB) en out, terminado en NUL.
 * @return Número de bytes copiados (0 si no hay log)
 */
size_t log_tail(char *out, size_t out_size);

/**
 * Cierra el fichero de log.
 */
void log_close(void);

#endif /* PSTORRENT5_LOG_H */
