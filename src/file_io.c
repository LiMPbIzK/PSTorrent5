#include "file_io.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static int safe_relative_path(const char *path)
{
    if (!path || !path[0] || path[0] == '/') return 0;
    for (const char *p = path; *p; p++) {
        if ((unsigned char)*p < 0x20 ||
            (*p == '.' && (p == path || p[-1] == '/') &&
             (p[1] == '/' || p[1] == '\0' ||
              (p[1] == '.' && (p[2] == '/' || p[2] == '\0')))))
            return 0;
    }
    return 1;
}

static int create_parent_directories(char *full_path)
{
    for (char *p = full_path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(full_path, 0755) < 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    return 0;
}

int file_writer_init(file_writer_t *fw, torrent_t *torrent,
                     const char *base_path)
{
    if (!fw || !torrent || !base_path) return -1;

    memset(fw, 0, sizeof(file_writer_t));

    fw->base_path = strdup(base_path);
    if (!fw->base_path) return -1;

    fw->torrent = torrent;
    fw->multi_file = torrent->is_multi_file;
    fw->fd = -1;

    if (!fw->multi_file) {
        /* Single file - open immediately */
        char full_path[1024];
        const char *relative_path = torrent->file_name
                                  ? torrent->file_name : torrent->name;
        if (!safe_relative_path(relative_path)) {
            free(fw->base_path);
            fw->base_path = NULL;
            errno = EINVAL;
            return -1;
        }
        int n = snprintf(full_path, sizeof(full_path), "%s/%s",
                         base_path, relative_path);
        if (n < 0 || (size_t)n >= sizeof(full_path)) {
            free(fw->base_path);
            fw->base_path = NULL;
            errno = ENAMETOOLONG;
            return -1;
        }

        fw->fd = open(full_path, O_WRONLY | O_CREAT, 0644);
        if (fw->fd < 0) {
            int saved_errno = errno;
            free(fw->base_path);
            fw->base_path = NULL;
            errno = saved_errno;
            return -1;
        }
    }

    return 0;
}

int file_writer_write_piece(file_writer_t *fw,
                            const unsigned char *data, size_t len,
                            size_t piece_index)
{
    if (!fw || !data || !fw->torrent) return -1;

    if (!fw->multi_file) {
        /* Single file: seek to piece position and write */
        off_t offset = (off_t)piece_index * fw->torrent->piece_length;
        if (lseek(fw->fd, offset, SEEK_SET) < 0) return -1;

        ssize_t written = write(fw->fd, data, len);
        if (written < 0 || (size_t)written != len) return -1;

        fw->written += len;
        return 0;
    }

    /* Multi-file: pieces may span file boundaries */
    off_t piece_start = (off_t)piece_index * fw->torrent->piece_length;
    off_t piece_end = piece_start + (off_t)len;
    size_t data_offset = 0;

    /* Find which file(s) this piece spans */
    off_t file_offset = 0;

    for (size_t i = 0; i < fw->torrent->num_files && data_offset < len; i++) {
        off_t file_start = file_offset;
        off_t file_end = file_offset + fw->torrent->files[i].length;

        if (piece_start < file_end && piece_end > file_start) {
            /* This piece (partially) belongs to this file */
            off_t write_start = (piece_start > file_start) ? piece_start - file_start : 0;
            off_t write_end = (piece_end < file_end) ? piece_end - file_start : fw->torrent->files[i].length;
            size_t write_len = (size_t)(write_end - write_start);

            if (write_len > 0) {
                /* Fichero no elegido: sus bytes del bloque se tiran, pero el
                 * cursor de lectura del bloque tiene que avanzar igual o los
                 * ficheros siguientes recibirian datos desplazados. */
                if (fw->file_selected && !fw->file_selected[i]) {
                    data_offset += write_len;
                } else {
                char full_path[1024];
                if (!safe_relative_path(fw->torrent->files[i].path)) {
                    errno = EINVAL;
                    return -1;
                }
                int n = snprintf(full_path, sizeof(full_path), "%s/%s",
                                 fw->base_path,
                                 fw->torrent->files[i].path);
                if (n < 0 || (size_t)n >= sizeof(full_path)) return -1;

                if (create_parent_directories(full_path) < 0) return -1;

                /* Open the file and seek to this piece's exact offset. */
                int fd = open(full_path, O_WRONLY | O_CREAT, 0644);
                if (fd < 0) return -1;

                /* Seek to the right position within the file */
                if (lseek(fd, write_start, SEEK_SET) < 0) {
                    close(fd);
                    return -1;
                }

                ssize_t written = write(fd, data + data_offset, write_len);
                close(fd);

                if (written < 0 || (size_t)written != write_len)
                    return -1;

                data_offset += write_len;
                fw->written += write_len;
                }
            }
        }

        file_offset = file_end;
    }

    return 0;
}

/**
 * Leer una pieza del disco (para verificar lo ya descargado al reanudar).
 *
 * Es el espejo de file_writer_write_piece(), incluida la seleccion de ficheros:
 * si la pieza toca un fichero no elegido, sus bytes no estan en disco y la pieza
 * no se puede verificar.
 *
 * @return 0 leida, -1 error o falta el fichero, -2 no verificable (toca un
 *         fichero no elegido: se deja como estaba)
 */
int file_writer_read_piece(file_writer_t *fw, unsigned char *data, size_t len,
                           size_t piece_index)
{
    int64_t piece_start;
    int64_t piece_end;
    int64_t file_offset = 0;
    size_t data_offset = 0;

    if (!fw || !data || !fw->torrent) return -1;

    piece_start = (int64_t)piece_index * fw->torrent->piece_length;
    piece_end = piece_start + (int64_t)len;

    if (!fw->multi_file) {
        char full_path[1024];
        const char *relative_path = fw->torrent->file_name
                                  ? fw->torrent->file_name : fw->torrent->name;
        int n = snprintf(full_path, sizeof(full_path), "%s/%s",
                         fw->base_path, relative_path);
        int fd;
        size_t got = 0;

        if (n < 0 || (size_t)n >= sizeof(full_path)) return -1;

        fd = open(full_path, O_RDONLY);
        if (fd < 0) return -1;
        if (lseek(fd, (off_t)piece_start, SEEK_SET) < 0) {
            close(fd);
            return -1;
        }
        while (got < len) {
            ssize_t r = read(fd, data + got, len - got);

            if (r <= 0) {
                close(fd);
                return -1;   /* el fichero es mas corto de lo que dice el estado */
            }
            got += (size_t)r;
        }
        close(fd);
        return 0;
    }

    for (size_t i = 0; i < fw->torrent->num_files && data_offset < len; i++) {
        int64_t file_start = file_offset;
        int64_t file_end = file_offset + fw->torrent->files[i].length;

        if (piece_start < file_end && piece_end > file_start) {
            int64_t read_start = (piece_start > file_start) ? piece_start - file_start : 0;
            int64_t read_end = (piece_end < file_end) ? piece_end - file_start
                                                      : fw->torrent->files[i].length;
            size_t read_len = (size_t)(read_end - read_start);

            if (read_len > 0) {
                if (fw->file_selected && !fw->file_selected[i])
                    return -2;   /* esa parte no esta en disco: no verificable */

                {
                    char full_path[1024];
                    int n = snprintf(full_path, sizeof(full_path), "%s/%s",
                                     fw->base_path, fw->torrent->files[i].path);
                    int fd;
                    size_t got = 0;

                    if (n < 0 || (size_t)n >= sizeof(full_path)) return -1;
                    fd = open(full_path, O_RDONLY);
                    if (fd < 0) return -1;
                    if (lseek(fd, (off_t)read_start, SEEK_SET) < 0) {
                        close(fd);
                        return -1;
                    }
                    while (got < read_len) {
                        ssize_t r = read(fd, data + data_offset + got, read_len - got);

                        if (r <= 0) {
                            close(fd);
                            return -1;
                        }
                        got += (size_t)r;
                    }
                    close(fd);
                }

                data_offset += read_len;
            }
        }

        file_offset = file_end;
    }

    return data_offset == len ? 0 : -1;
}

void file_writer_finish(file_writer_t *fw){
    if (!fw) return;

    if (fw->fd >= 0) {
        close(fw->fd);
        fw->fd = -1;
    }
}

void file_writer_destroy(file_writer_t *fw)
{
    if (!fw) return;
    file_writer_finish(fw);
    free(fw->base_path);
    memset(fw, 0, sizeof(file_writer_t));
    fw->fd = -1;
}
