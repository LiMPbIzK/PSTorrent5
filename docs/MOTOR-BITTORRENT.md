# Motor BitTorrent de PSTorrent5

Todo lo que hay aquí está **medido**, no supuesto. Fecha de las mediciones: 16 y
17 de octubre de 2026, red del usuario (IP pública `89.35.62.35`, sin IPv6).

## 1. Cómo se descubren peers

Tres vías, todas alimentando **el mismo pool de candidatos** por torrent
(`peer_pool`, 256 direcciones):

| Vía | Fichero | Estado |
|---|---|---|
| Trackers del `.torrent` (HTTP/HTTPS/UDP) | `src/tracker.c` | Funciona |
| **Lista extra de trackers UDP** | `tracker_extra_list()` en `src/tracker.c` | Funciona: 30/16/11 peers |
| **PEX** (BEP-11 sobre BEP-10) | `src/peer_wire.c` + `peer_handle_extended()` | Funciona |
| **DHT** (BEP-5/KRPC) | `src/dht.c` | Bootstrapea (122 nodos) pero **los routers están filtrados aquí**: 0 peers |

**Cursor de rondas (`announce_cursor`)**: imprescindible. Con un máximo de 3
trackers por ronda, si los primeros de la lista contestan bien nunca entran en
backoff y acaparan todas las rondas; los extra no se probarían jamás.

## 2. Los dos hechos que explican todas las lentitudes

**a) El tracker oficial puede estar "clavado" a un peer.** Ocho announces a
`torrent.ubuntu.com` con `peer_id`, puerto y `numwant` distintos devuelven
**siempre el mismo** `185.125.190.59:6908` (o nada) con 293 seeders en el
enjambre. Es un tracker de puntero: reparte uno y confía en PEX/DHT para el
resto. Y ese peer no soporta BEP-10, así que con él no hay PEX posible.

**b) La mayoría de peers está detrás de NAT.** Muestra de 30 peers de los
trackers UDP: **26 no aceptan TCP**, 4 aceptan y **solo 1 completa el
handshake**. No es la red del usuario: `portquiz.net` responde en los puertos
20015 y 51413, o sea que el TCP saliente no está filtrado.

Consecuencia de diseño: **el candidato a peer hay que probarlo rápido y en
paralelo**, porque la mayoría muere.

## 3. Regla de oro del protocolo de extensión

Un mensaje de extensión (id 20) **solo** se manda a un peer que haya puesto el
bit `0x10` en `reserved[5]` de su handshake. A quien no lo ha anunciado le
parece una violación de protocolo y **cierra la conexión**. Reproducido contra
un peer real: con solo `interested` la conexión vive; añadiendo el mensaje de
extensión, cierra en 0,14 s.

Corolario sobre el bencode: va con longitudes por delante. Un solo dígito de más
convierte el mensaje en basura y el peer cierra **sin decir nada**. Ya pasó dos
veces; ahora hay autopruebas al arrancar (`PEX ready: ut_pex id 1`,
`dht: query format ok`) que parsean el mensaje que genera el propio código.

## 4. Máquina de estados de un peer

```
IDLE ──peer_start_connect()──> CONNECTING ──poll OK──> HANDSHAKING ──68 B validos──> READY
  ^                                 |                        |
  └────────── peer_close() ─────────┴────────────────────────┘
```

* `net_tcp_connect_start()` deja el socket **no bloqueante**; `net_tcp_connect_poll()`
  dice si ya conectó. Hasta **32 conexiones en vuelo**.
* El handshake se envía y se lee **por trozos**, sin bloquear.
* Al pasar a READY se vuelve a modo bloqueante con `SO_RCVTIMEO` de 8 s: el
  cuerpo de los mensajes se lee con `select()` por delante (concesión
  consciente, ver `ARCHITECTURE.md`).
* Solo los READY cuentan como activos y solo a ellos se les pide nada.

## 5. Descarga en paralelo

Hasta **4 piezas en curso** (`inflight_t`), **una por peer**, cada una con su
ventana de 16 peticiones de 16 KB. El buffer se reserva al usarse y el número de
ranuras se ajusta al tamaño de pieza (4 con 256 KB, 1 con 16 MB). Una ranura sin
recibir nada en 120 s se suelta para que la coja otro peer.

Escribir fuera de orden es seguro: `file_writer_write_piece()` posiciona con
`lseek` por índice de pieza.

## 6. Cómo depurar (sondas externas, sin tocar la consola)

Están en el directorio temporal del entorno; son scripts de Node que reproducen
lo que hace el cliente:

| Sonda | Para qué |
|---|---|
| `tracker-probe.js` | Cuántos peers da un tracker (y el tamaño real del enjambre) |
| `tracker-variants.js` | Probar `numwant`, `compact`, HTTP/HTTPS, IPv6 |
| `udp-tracker.js` | Trackers UDP (BEP-15): connect + announce |
| `peer-quality.js` | **La más útil**: de N peers, cuántos aceptan TCP, completan handshake y tienen datos |
| `peer-probe.js` | Comparar variantes de lo que enviamos al conectar |
| `dht-probe.js` | Bootstrap y `get_peers` contra la red DHT real |

## 7. Qué mirar en el log del payload

```
engine: PSTorrent5 1.1.0 (build ...)        <- qué ELF corre de verdad
PEX ready: ut_pex id 1                      <- BEP-10 bien formado
dht: query format ok                        <- KRPC bien formado
announce: UDP tracker.opentrackr.org:1337 -> 30 peer(s), 16 seeder(s)
peer 0 supports ut_pex (id 1)
peer 0 ut_pex: 2 new peer(s) (pool=12)
peer connect failed: x.x.x.x:port           <- RST (rechazo); los timeouts no se trazan
piece 20/23844 ok (5.0 MB downloaded)
progress: #0 '...' downloading 0.9% (52.4/5960.8 MB) 640 KB/s peers=4
```
