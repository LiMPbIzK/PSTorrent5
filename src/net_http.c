#include "net_http.h"
#include "ui.h"

#include <string.h>

/*
 * Declaraciones de la pila HTTP/SSL de la consola.
 *
 * El SDK trae las librerias (target/lib/libSceHttp2.so, libSceSsl.so,
 * libSceNet.so) pero no cabeceras, asi que las firmas se declaran aqui; estan
 * tomadas de samples/http2_get del propio ps5-payload-sdk, que es el uso
 * oficial. Los timeouts van en MICROSEGUNDOS (convencion de la API sceHttp).
 */
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int memid);

int sceSslInit(size_t size);
int sceSslTerm(int sslid);

int sceHttp2Init(int memid, int sslid, size_t size, int flags);
int sceHttp2Term(int ctx);
int sceHttp2CreateTemplate(int ctx, const char *agent, int max_connections,
                           int auto_proxy);
int sceHttp2DeleteTemplate(int tmpl);
int sceHttp2CreateRequestWithURL(int tmpl, const char *method, const char *url,
                                 uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2SendRequest(int req, const void *data, size_t size);
int sceHttp2GetStatusCode(int req, int *status);
int sceHttp2ReadData(int req, void *buf, size_t size);
int sceHttp2SetConnectTimeOut(int id, unsigned int usec);
int sceHttp2SetRecvTimeOut(int id, unsigned int usec);
int sceHttp2SetSendTimeOut(int id, unsigned int usec);
int sceHttp2SetResolveTimeOut(int id, unsigned int usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceHttp2SetInflateGZIPEnabled(int id, int enable);

/* La pila comparte hilo con el motor y con el panel web: ningun paso debe
 * bloquear mas de esto. */
#define NET_HTTP_TIMEOUT_USEC (6u * 1000u * 1000u)

#define NET_POOL_SIZE  (64 * 1024)
#define NET_SSL_SIZE   (512 * 1024)
#define NET_HTTP_SIZE  (512 * 1024)

static int net_mem_id = -1;
static int net_ssl_id = -1;
static int net_ctx_id = -1;
static int net_tmpl_id = -1;
static int net_http_ready = 0;

static void http_apply_timeouts(int id)
{
    int rc;

    rc = sceHttp2SetResolveTimeOut(id, NET_HTTP_TIMEOUT_USEC);
    if (rc) ui_log("http: SetResolveTimeOut -> 0x%x", rc);
    rc = sceHttp2SetConnectTimeOut(id, NET_HTTP_TIMEOUT_USEC);
    if (rc) ui_log("http: SetConnectTimeOut -> 0x%x", rc);
    rc = sceHttp2SetSendTimeOut(id, NET_HTTP_TIMEOUT_USEC);
    if (rc) ui_log("http: SetSendTimeOut -> 0x%x", rc);
    rc = sceHttp2SetRecvTimeOut(id, NET_HTTP_TIMEOUT_USEC);
    if (rc) ui_log("http: SetRecvTimeOut -> 0x%x", rc);
    rc = sceHttp2SetAutoRedirect(id, 1);
    if (rc) ui_log("http: SetAutoRedirect -> 0x%x", rc);
    rc = sceHttp2SetInflateGZIPEnabled(id, 1);
    if (rc) ui_log("http: SetInflateGZIPEnabled -> 0x%x", rc);
}

int net_http_init(const char *user_agent)
{
    if (net_http_ready)
        return 0;

    /* Non-fatal: si el servicio ya estaba inicializado o no esta disponible,
     * el motor sigue con su propio cliente HTTP. */
    if (sceNetInit() != 0)
        ui_log("http: sceNetInit returned non-zero (continuing)");

    net_mem_id = sceNetPoolCreate("pstorrent5", NET_POOL_SIZE, 0);
    if (net_mem_id < 0) {
        ui_log("http: sceNetPoolCreate failed (0x%x)", net_mem_id);
        return -1;
    }

    net_ssl_id = sceSslInit(NET_SSL_SIZE);
    if (net_ssl_id < 0) {
        ui_log("http: sceSslInit failed (0x%x)", net_ssl_id);
        return -1;
    }

    net_ctx_id = sceHttp2Init(net_mem_id, net_ssl_id, NET_HTTP_SIZE, 1);
    if (net_ctx_id < 0) {
        ui_log("http: sceHttp2Init failed (0x%x)", net_ctx_id);
        return -1;
    }

    net_tmpl_id = sceHttp2CreateTemplate(net_ctx_id,
                                         user_agent ? user_agent
                                                    : "PSTorrent5",
                                         3, 1);
    if (net_tmpl_id < 0) {
        ui_log("http: sceHttp2CreateTemplate failed (0x%x)", net_tmpl_id);
        return -1;
    }

    http_apply_timeouts(net_tmpl_id);
    net_http_ready = 1;
    ui_log("http: console HTTP stack ready (HTTPS supported)");
    return 0;
}

int net_http_available(void)
{
    return net_http_ready;
}

/**
 * Autoprueba de la pila, una vez al arrancar: separa "la pila no funciona en
 * este entorno" de "esta URL concreta falla". La primera comprueba HTTP y la
 * segunda HTTPS, de modo que un fallo solo en la segunda senala al TLS/DNS y un
 * fallo en ambas a la propia pila.
 */
void net_http_selftest(void)
{
    unsigned char buf[2048];
    int status;
    int got;

    if (!net_http_ready)
        return;

    status = 0;
    got = net_http_get("http://example.com/", buf, sizeof(buf), &status);
    ui_log("http selftest: HTTP  example.com -> %d bytes, status %d",
           got, status);

    status = 0;
    got = net_http_get("https://example.com/", buf, sizeof(buf), &status);
    ui_log("http selftest: HTTPS example.com -> %d bytes, status %d",
           got, status);
}

int net_http_get(const char *url, unsigned char *out, size_t out_size,
                 int *status)
{
    int request;
    int code = 0;
    int read_bytes;
    size_t total = 0;

    if (!net_http_ready || !url || !out || out_size < 2)
        return -1;

    request = sceHttp2CreateRequestWithURL(net_tmpl_id, "GET", url, 0);
    if (request < 0) {
        ui_log("http: cannot create request for %s (0x%x)", url, request);
        return -1;
    }

    http_apply_timeouts(request);

    {
        int rc = sceHttp2SendRequest(request, NULL, 0);
        if (rc != 0) {
            /* El codigo distingue: 0x80431xxx = HTTP (parametros, DNS,
             * conexion), 0x80435xxx = SSL/certificados. */
            ui_log("http: send failed for %s (0x%x)", url, rc);
            sceHttp2DeleteRequest(request);
            return -1;
        }
    }

    {
        int rc = sceHttp2GetStatusCode(request, &code);
        if (rc != 0)
            ui_log("http: GetStatusCode failed (0x%x)", rc);
        else if (status)
            *status = code;
    }

    /* Se lee el cuerpo tanto en respuestas correctas como en errores: el
     * tracker explica el motivo dentro del propio bencode. */
    while (total + 1 < out_size &&
           (read_bytes = sceHttp2ReadData(request, out + total,
                                          out_size - total - 1)) > 0) {
        total += (size_t)read_bytes;
    }
    out[total] = '\0';

    sceHttp2DeleteRequest(request);
    return (int)total;
}

void net_http_shutdown(void)
{
    if (net_tmpl_id != -1) {
        sceHttp2DeleteTemplate(net_tmpl_id);
        net_tmpl_id = -1;
    }
    if (net_ctx_id != -1) {
        sceHttp2Term(net_ctx_id);
        net_ctx_id = -1;
    }
    if (net_ssl_id != -1) {
        sceSslTerm(net_ssl_id);
        net_ssl_id = -1;
    }
    if (net_mem_id != -1) {
        sceNetPoolDestroy(net_mem_id);
        net_mem_id = -1;
    }
    net_http_ready = 0;
}
