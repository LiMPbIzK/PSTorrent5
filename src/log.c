#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Rotación simple: por encima de este tamaño el fichero pasa a <ruta>.1 */
#define LOG_MAX_BYTES  (256 * 1024)
/* Tamaño del fragmento que sirve GET /api/log */
#define LOG_TAIL_BYTES (48 * 1024)

static FILE *log_file = NULL;
static char  log_file_path[LOG_PATH_MAX] = {0};

/* Orden de preferencia. /data es el almacenamiento interno persistente. */
static const char *candidate_dirs[] = {
    "/data/pstorrent5",
    "/data",
    "/tmp",
};

static int ensure_dir(const char *dir)
{
    if (mkdir(dir, 0777) == 0)
        return 0;
    return errno == EEXIST ? 0 : -1;
}

int log_init(void)
{
    size_t i;

    if (log_file)
        return 0;

    for (i = 0; i < sizeof(candidate_dirs) / sizeof(candidate_dirs[0]); i++) {
        char candidate[LOG_PATH_MAX];
        struct stat st;
        FILE *handle;

        if (ensure_dir(candidate_dirs[i]) < 0)
            continue;

        snprintf(candidate, sizeof(candidate), "%s/pstorrent5.log",
                 candidate_dirs[i]);

        handle = fopen(candidate, "a");
        if (!handle)
            continue;

        /* Si el log anterior es grande, se aparta en vez de crecer sin fin. */
        if (stat(candidate, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
            char rotated[LOG_PATH_MAX + 4];

            fclose(handle);
            snprintf(rotated, sizeof(rotated), "%s.1", candidate);
            remove(rotated);
            if (rename(candidate, rotated) != 0) {
                /* Sin permiso para rotar: se sigue añadiendo al mismo fichero */
            }
            handle = fopen(candidate, "a");
            if (!handle)
                continue;
        }

        log_file = handle;
        strncpy(log_file_path, candidate, sizeof(log_file_path) - 1);
        log_file_path[sizeof(log_file_path) - 1] = '\0';
        return 0;
    }

    return -1;
}

const char *log_path(void)
{
    return log_file ? log_file_path : "";
}

void log_append(const char *text)
{
    char stamp[16];
    struct tm *tm_now;
    time_t now;

    if (!log_file || !text)
        return;

    now = time(NULL);
    tm_now = localtime(&now);
    if (tm_now) {
        strftime(stamp, sizeof(stamp), "%H:%M:%S", tm_now);
    } else {
        strncpy(stamp, "--:--:--", sizeof(stamp));
    }

    fprintf(log_file, "[%s] %s\n", stamp, text);

    /* Sin buffer: si el payload muere de golpe, lo escrito sigue en el
     * fichero, que es justo lo que hace falta para diagnosticar. */
    fflush(log_file);
}

size_t log_tail(char *out, size_t out_size)
{
    FILE *handle;
    long size;
    size_t start;
    size_t read_bytes;

    if (!out || out_size < 2)
        return 0;

    out[0] = '\0';

    if (log_file_path[0] == '\0')
        return 0;

    handle = fopen(log_file_path, "r");
    if (!handle)
        return 0;

    if (fseek(handle, 0, SEEK_END) != 0) {
        fclose(handle);
        return 0;
    }

    size = ftell(handle);
    if (size < 0) {
        fclose(handle);
        return 0;
    }

    start = (size > (long)LOG_TAIL_BYTES) ? (size_t)(size - LOG_TAIL_BYTES) : 0;
    if (fseek(handle, (long)start, SEEK_SET) != 0) {
        fclose(handle);
        return 0;
    }

    read_bytes = fread(out, 1, out_size - 1, handle);
    out[read_bytes] = '\0';
    fclose(handle);

    return read_bytes;
}

void log_close(void)
{
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}
