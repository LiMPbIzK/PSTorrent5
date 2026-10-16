/**
 * PSTorrent5 - Web interface + multi-torrent engine
 *
 * BitTorrent client for PlayStation 5 with embedded web UI.
 * Access the web interface at http://PS5_IP:12389/
 *
 * Fork of jfcardososantos/PS5Torrent (GPL-3.0). See ARCHITECTURE.md.
 *
 * Features:
 *   - Add torrents via magnet links or .torrent file upload
 *   - Choose from known PS5 storage paths (USB, NVMe, internal, etc.)
 *   - Multi-torrent management with simultaneous downloads
 *   - Real-time progress via web UI (es/en/pt)
 *   - klog console logging
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "sha1.h"
#include "bencode.h"
#include "torrent.h"
#include "net_utils.h"
#include "tracker.h"
#include "peer_wire.h"
#include "piece_mgr.h"
#include "file_io.h"
#include "ui.h"
#include "http_server.h"
#include "torrent_mgr.h"
#include "storage_paths.h"
#include "ps5_jailbreak.h"
#include "ps5_browser.h"
#include "web_content.h"
#include "version.h"
#include "log.h"
#include "net_http.h"

static int etahen_jailbreak_active = 0;

static void json_escape_text(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t pos = 0;

    if (!output || output_size == 0) return;
    if (!input) input = "";

    for (const unsigned char *p = (const unsigned char *)input;
         *p && pos + 1 < output_size; p++) {
        unsigned char c = *p;
        if (c == '\"' || c == '\\') {
            if (pos + 2 >= output_size) break;
            output[pos++] = '\\';
            output[pos++] = (char)c;
        } else if (c < 0x20) {
            if (pos + 6 >= output_size) break;
            output[pos++] = '\\';
            output[pos++] = 'u';
            output[pos++] = '0';
            output[pos++] = '0';
            output[pos++] = hex[c >> 4];
            output[pos++] = hex[c & 15];
        } else {
            output[pos++] = (char)c;
        }
    }
    output[pos] = '\0';
}

static int build_save_path(const char *base_path,
                           char *save_path, size_t save_path_size)
{
    size_t base_len;
    int n;

    if (!base_path || base_path[0] != '/' || !save_path ||
        save_path_size == 0)
        return -1;

    /* Keep a custom destination absolute and canonical enough that it cannot
     * escape into an unexpected directory through dot path components. */
    for (const char *p = base_path; *p; p++) {
        if ((unsigned char)*p < 0x20 ||
            (*p == '.' && (p == base_path || p[-1] == '/') &&
             (p[1] == '/' || p[1] == '\0' ||
              (p[1] == '.' && (p[2] == '/' || p[2] == '\0')))))
            return -1;
    }

    base_len = strlen(base_path);
    while (base_len > 1 && base_path[base_len - 1] == '/')
        base_len--;

    n = snprintf(save_path, save_path_size, "%.*s%storrents/",
                 (int)base_len, base_path, base_len == 1 ? "" : "/");
    return n >= 0 && (size_t)n < save_path_size ? 0 : -1;
}

/**
 * Build a JSON error response: the human-readable message plus a stable code
 * the web panel translates (see the i18n table in src/web/index.html).
 */
static http_response_t *json_error(int status, const char *code,
                                   const char *message)
{
    char body[512];
    char escaped[384];
    int n;

    json_escape_text(message ? message : "", escaped, sizeof(escaped));
    n = snprintf(body, sizeof(body),
                 "{\"success\":false,\"error\":\"%s\",\"error_code\":\"%s\"}",
                 escaped, code ? code : "");
    if (n < 0) n = 0;

    return http_response_new(status, "application/json", body, (size_t)n);
}

static http_response_t *torrent_start_error_response(int index,
                                                     int remove_torrent)
{
    managed_torrent_t *mt = torrent_mgr_get(index);
    const char *message = mt && mt->error_msg[0]
                        ? mt->error_msg : "The torrent could not be started";
    const char *code = mt && mt->error_code[0] ? mt->error_code : "start_failed";
    int status = mt && mt->error_type == TERR_WRITE_FAILED ? 403 : 400;
    http_response_t *response;

    if (status == 403 && !etahen_jailbreak_active) {
        char detailed[512];
        snprintf(detailed, sizeof(detailed),
                 "%s. On etaHEN, enable Network and Legacy CMD server and "
                 "reopen the app", message);
        response = json_error(status, code, detailed);
    } else {
        response = json_error(status, code, message);
    }

    if (remove_torrent)
        torrent_mgr_remove(index);

    return response;
}

/**
 * Web interface handler: serves the main HTML page.
 */
static http_response_t *handle_web_index(const http_request_t *req)
{
    return http_response_new(200, "text/html; charset=utf-8",
                             web_index_html, strlen(web_index_html));
}

/**
 * API: Get torrent status list.
 */
static http_response_t *handle_api_torrents(const http_request_t *req)
{
    char *json = malloc(16384);
    if (!json) return http_response_new(500, "application/json", "{}", 2);

    torrent_mgr_status_json(json, 16384);
    http_response_t *resp = http_response_new(200, "application/json", json, strlen(json));
    free(json);
    return resp;
}

/**
 * API: Add torrent via magnet link (POST form data).
 */
static http_response_t *handle_api_add(const http_request_t *req)
{
    if (req->method != HTTP_POST || !req->body) {
        return json_error(400, "post_required", "POST required");
    }

    char magnet[2048] = {0};
    char path[512] = {0};

    http_get_param(req->body, "magnet", magnet, sizeof(magnet));
    http_get_param(req->body, "path", path, sizeof(path));

    if (!magnet[0]) {
        return json_error(400, "magnet_required", "Magnet link required");
    }

    if (!path[0])
        strncpy(path, storage_paths_get_default(), sizeof(path) - 1);

    char save_path[512];
    if (build_save_path(path, save_path, sizeof(save_path)) < 0)
        return json_error(400, "path_invalid", "Invalid path");

    int idx = torrent_mgr_add_magnet(magnet, save_path);
    if (idx < 0) {
        return json_error(400, "magnet_invalid",
                          "Invalid magnet link or too many torrents");
    }

    /* Start download automatically */
    if (torrent_mgr_start(idx) < 0)
        return torrent_start_error_response(idx, 1);

    managed_torrent_t *mt = torrent_mgr_get(idx);
    char resp_json[512];
    char escaped_name[384];
    json_escape_text(mt ? mt->name : "Unknown",
                     escaped_name, sizeof(escaped_name));
    int n = snprintf(resp_json, sizeof(resp_json),
        "{\"success\":true,\"id\":%d,\"name\":\"%s\"}",
        idx, escaped_name);

    return http_response_new(200, "application/json", resp_json, (size_t)n);
}

/**
 * API: Upload .torrent file (POST multipart).
 */
static http_response_t *handle_api_upload(const http_request_t *req)
{
    if (req->method != HTTP_POST || !req->body || req->body_len == 0) {
        return json_error(400, "file_required", "File required");
    }

    const char *file_data = NULL;
    size_t file_len = 0;
    char filename[256] = {0};
    char path[512] = {0};

    /* Try multipart form data */
    if (!http_get_upload(req->body, req->body_len, "torrent",
                         &file_data, &file_len, filename, sizeof(filename))) {
        /* URL-encoded form: the simple base64 path is not implemented, so the
         * field is only checked for presence. No large stack buffer here: this
         * handler shares the payload's thread stack with the whole engine. */
        char torrent_data[64] = {0};
        http_get_param(req->body, "torrent_data", torrent_data,
                       sizeof(torrent_data));
        http_get_param(req->body, "path", path, sizeof(path));

        if (!torrent_data[0])
            return json_error(400, "no_file", "No torrent file found");

        return json_error(400, "multipart_required",
                          "Use multipart upload instead");
    }

    http_get_multipart_field(req->body, req->body_len, "path",
                             path, sizeof(path));

    if (!path[0])
        strncpy(path, storage_paths_get_default(), sizeof(path) - 1);

    if (!file_data || file_len == 0)
        return json_error(400, "empty_file", "Empty file");

    char save_path[512];
    if (build_save_path(path, save_path, sizeof(save_path)) < 0)
        return json_error(400, "path_invalid", "Invalid path");

    int idx = torrent_mgr_add_raw((const unsigned char *)file_data, file_len, save_path);
    if (idx < 0)
        return json_error(400, "invalid_torrent", "Invalid torrent file");

    /* Start download automatically */
    if (torrent_mgr_start(idx) < 0)
        return torrent_start_error_response(idx, 1);

    managed_torrent_t *mt = torrent_mgr_get(idx);
    char resp_json[512];
    char escaped_name[384];
    json_escape_text(mt ? mt->name : "Unknown",
                     escaped_name, sizeof(escaped_name));
    int n = snprintf(resp_json, sizeof(resp_json),
        "{\"success\":true,\"id\":%d,\"name\":\"%s\"}",
        idx, escaped_name);

    return http_response_new(200, "application/json", resp_json, (size_t)n);
}

/**
 * API: Torrent action (start/stop/remove).
 */
static http_response_t *handle_api_action(const http_request_t *req)
{
    if (req->method != HTTP_POST) {
        return json_error(400, "post_required", "POST required");
    }

    /* Parse path: /api/torrents/<id>/<action> */
    int id = -1;
    char action[16] = {0};

    /* Extract ID and action from path */
    const char *p = req->path;
    /* Skip /api/torrents/ (14 characters) */
    p += 14;

    /* Read digits for ID */
    while (*p >= '0' && *p <= '9') {
        if (id < 0) id = 0;
        id = id * 10 + (*p - '0');
        p++;
    }

    if (id < 0 || *p != '/') {
        return json_error(400, "path_invalid", "Invalid path");
    }

    p++; // skip '/'
    strncpy(action, p, sizeof(action) - 1);

    int result = -1;
    if (strcmp(action, "start") == 0)
        result = torrent_mgr_start(id);
    else if (strcmp(action, "stop") == 0) {
        torrent_mgr_stop(id);
        result = 0;
    } else if (strcmp(action, "remove") == 0) {
        torrent_mgr_remove(id);
        result = 0;
    }

    if (result == 0) {
        char resp[128];
        int n = snprintf(resp, sizeof(resp),
                         "{\"success\":true,\"action\":\"%s\",\"id\":%d}", action, id);
        return http_response_new(200, "application/json", resp, (size_t)n);
    } else {
        if (strcmp(action, "start") == 0 && id >= 0 && torrent_mgr_get(id))
            return torrent_start_error_response(id, 0);
        return json_error(400, "action_invalid", "Invalid action");
    }
}

/**
 * API: Get available paths.
 */
static http_response_t *handle_api_paths(const http_request_t *req)
{
    storage_path_t paths[MAX_STORAGE_PATHS];
    size_t count = 0;
    storage_paths_get(paths, &count);

    char *json = malloc(4096);
    if (!json) return http_response_new(500, "application/json", "{}", 2);

    size_t pos = 0;
    pos += snprintf(json + pos, 4096 - pos, "{\"paths\":[");

    for (size_t i = 0; i < count; i++) {
        if (i > 0) pos += snprintf(json + pos, 4096 - pos, ",");
        int exists = storage_path_exists(paths[i].path);
        int writable = exists && storage_path_is_writable(paths[i].path);
        pos += snprintf(json + pos, 4096 - pos,
            "{\"path\":\"%s\",\"label\":\"%s\",\"icon\":\"%s\","
            "\"exists\":%d,\"writable\":%d,\"removable\":%d,\"default\":%d}",
            paths[i].path, paths[i].label, paths[i].icon,
            exists, writable, paths[i].is_removable, paths[i].is_default);
    }

    pos += snprintf(json + pos, 4096 - pos, "]}");

    http_response_t *resp = http_response_new(200, "application/json", json, pos);
    free(json);
    return resp;
}

/**
 * API: Get server status.
 */
static http_response_t *handle_api_status(const http_request_t *req)
{
    char resp[1024];
    int n = snprintf(resp, sizeof(resp),
        "{\"status\":\"running\",\"version\":\"" PSTORRENT5_VERSION "\",\"jailbroken\":%s,"
        "\"torrents\":%d,\"capacity\":%d,\"log_path\":\"%s\"}",
        etahen_jailbreak_active ? "true" : "false",
        torrent_mgr_count(), torrent_mgr_capacity(), log_path());
    return http_response_new(200, "application/json", resp, (size_t)n);
}

/**
 * API: Serve the tail of the payload log as text/plain.
 *
 * The log also lives in a file on the console (see log_init), so it can be
 * read over FTP; this endpoint is the quick path from the browser:
 * http://<PS5-IP>:12389/api/log
 */
static http_response_t *handle_api_log(const http_request_t *req)
{
    size_t capacity = 64 * 1024;
    char *buffer = malloc(capacity);
    size_t length;
    http_response_t *response;

    (void)req;

    if (!buffer)
        return json_error(500, "no_memory", "Out of memory");

    length = log_tail(buffer, capacity);
    response = http_response_new(200, "text/plain; charset=utf-8",
                                 buffer, length);
    free(buffer);
    return response;
}

/**
 * Calculate monotonic time in milliseconds.
 */
static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/**
 * Get a network interface's IPv4 address using POSIX socket API.
 * Works on PS5/FreeBSD without shell commands.
 * Returns 0 if IP found, -1 if not.
 */
static int get_local_ip(char *buf, size_t bufsz)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        strncpy(buf, "192.168.x.x", bufsz - 1);
        return -1;
    }

    struct ifconf ifc;
    struct ifreq ifr[16];

    ifc.ifc_len = sizeof(ifr);
    ifc.ifc_req = ifr;

    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0) {
        close(fd);
        strncpy(buf, "192.168.x.x", bufsz - 1);
        return -1;
    }

    int found = -1;
    int num_if = ifc.ifc_len / sizeof(struct ifreq);

    for (int i = 0; i < num_if; i++) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr[i].ifr_addr;
        uint32_t ip = ntohl(sin->sin_addr.s_addr);

        /* Skip loopback (127.x.x.x) */
        if ((ip >> 24) == 127) continue;
        /* Skip link-local (169.254.x.x) */
        if ((ip >> 16) == 0xA9FE) continue;
        /* Skip zeros */
        if (ip == 0) continue;

        /* Found a valid IP */
        snprintf(buf, bufsz, "%d.%d.%d.%d",
                 (int)(ip >> 24) & 0xFF,
                 (int)(ip >> 16) & 0xFF,
                 (int)(ip >> 8) & 0xFF,
                 (int)(ip & 0xFF));
        found = 0;
        break;
    }

    close(fd);

    if (found < 0) {
        strncpy(buf, "192.168.x.x", bufsz - 1);
    }

    return found;
}

/**
 * Entry point called by ps5-payload-sdk CRT.
 * The payload loader does not guarantee a valid envp third argument, so the
 * entry point deliberately uses the portable no-argument form.
 */
int main(void)
{
    /* Initialize subsystems */
    ui_init();

    /* El log a fichero va primero, para que quede constancia de todo lo demas */
    if (log_init() == 0)
        ui_log("Log file: %s", log_path());
    else
        ui_error("Could not open a log file; logging to klog only");

    ui_print_banner();

    /* The system browser API requires UserService to be initialized first. */
    int browser_ready = ps5_browser_init() == 0;

    /* Initialize networking first */
    if (net_init() < 0) {
        ui_error("Failed to initialize networking");
        ui_notify("PSTorrent5 did not start: network setup failed.");
        ps5_browser_shutdown();
        return 1;
    }

    /* Console HTTP/SSL stack: needed for https:// trackers (and later for the
     * self-updater). Non-fatal: without it only plain HTTP works. */
    net_http_init("PSTorrent5/" PSTORRENT5_VERSION);

    /* fPKGs start sandboxed. etaHEN can grant filesystem access through its
     * local command service; payloads sent to an ELF loader may already have
     * the required privileges and continue normally when the service is off. */
    if (ps5_request_jailbreak() == 0) {
        etahen_jailbreak_active = 1;
        ui_log("etaHEN jailbreak request accepted");
    } else {
        ui_log("etaHEN jailbreak service unavailable; checking current access");
    }

    /* Initialize HTTP server */
    ui_log("Initializing web interface on port %d...", HTTP_PORT);

    if (http_server_init(HTTP_PORT) < 0) {
        ui_error("Failed to start HTTP server on port %d", HTTP_PORT);
        ui_error("Make sure port %d is not in use.", HTTP_PORT);
        ui_notify("PSTorrent5 did not start: port %d is already in use.",
                  HTTP_PORT);
        net_cleanup();
        ps5_browser_shutdown();
        return 1;
    }

    /* Register specific API prefixes before the catch-all web route. */
    http_server_register("/api/torrents/add", handle_api_add);
    http_server_register("/api/torrents/upload", handle_api_upload);
    http_server_register("/api/torrents/", handle_api_action);
    http_server_register("/api/torrents", handle_api_torrents);
    http_server_register("/api/paths", handle_api_paths);
    http_server_register("/api/status", handle_api_status);
    http_server_register("/api/log", handle_api_log);

    /* Catch-all web UI handler must remain last. */
    http_server_register("/", handle_web_index);

    /* Initialize torrent manager */
    torrent_mgr_init();

    /* Show available storage paths */
    storage_path_t paths[MAX_STORAGE_PATHS];
    size_t num_paths = 0;
    storage_paths_get(paths, &num_paths);

    ui_log("Available storage paths:");

    char default_path[256] = {0};
    for (size_t i = 0; i < num_paths; i++) {
        int exists = storage_path_exists(paths[i].path);
        int writable = exists && storage_path_is_writable(paths[i].path);
        ui_log("  %s %s %s %s",
               paths[i].icon,
               paths[i].label,
               paths[i].path,
               writable ? "✅ Writable" :
               (exists ? "⚠️ Read-only" : "❌ Not mounted"));

        /* Pick first writable path */
        if (writable && !default_path[0]) {
            strncpy(default_path, paths[i].path, sizeof(default_path) - 1);
        }
    }

    /* Get local IP for user info */
    char local_ip[64] = "<unknown>";
    get_local_ip(local_ip, sizeof(local_ip));

    ui_log("========================================");
    ui_log("  🎮 PSTorrent5 is running!");
    ui_log("  ");
    ui_log("  Open this URL in your browser:");
    ui_log("  http://%s:%d/", local_ip, HTTP_PORT);
    ui_log("  ");
    ui_log("  📥 Use the web UI to add torrents");
    ui_log("  💾 Save to USB/NVMe/internal storage");
    ui_log("========================================");
    ui_log("");
    ui_log("Status: HTTP server active | Torrent engine ready");

    ui_notify("PSTorrent5 is running\nPanel: http://%s:%d/",
              local_ip, HTTP_PORT);

    if (browser_ready &&
        ps5_browser_open("http://127.0.0.1:12389/") == 0)
        ui_log("Opening the web interface on the PS5...");
    else {
        ui_error("Could not open the web interface automatically");
        ui_notify("Open the browser at http://127.0.0.1:%d/", HTTP_PORT);
    }

    if (default_path[0]) {
        ui_log("💡 Storage detected at: %s", default_path);
    }

    /* Main event loop */
    uint64_t last_status = 0;
    int running = 1;

    while (running) {
        uint64_t now = get_time_ms();

        /* Poll HTTP server (5ms timeout - quick check) */
        int ret = http_server_poll(5);
        if (ret < 0) {
            ui_error("HTTP server error, shutting down...");
            break;
        }

        /* Tick torrent manager */
        int active = torrent_mgr_tick();

        /* Print status every 5 seconds. Se imprime tambien cuando hay torrents
         * parados o en error: sirve de latido para saber que el bucle sigue
         * vivo aunque no haya trafico. */
        if (now - last_status > 5000) {
            last_status = now;
            if (active > 0 || torrent_mgr_count() > 0) {
                ui_log("heartbeat: active=%d total=%d/%d",
                       active, torrent_mgr_count(), torrent_mgr_capacity());
            }
        }

        /* Small delay to prevent busy-waiting */
        usleep(10000); // 10ms
    }

    /* Cleanup */
    ui_log("Shutting down...");
    torrent_mgr_shutdown();
    http_server_shutdown();
    net_http_shutdown();
    net_cleanup();
    ps5_browser_shutdown();
    log_close();

    ui_log("PSTorrent5 terminated.");
    return 0;
}
