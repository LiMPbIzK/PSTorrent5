# 📋 Contenido. Lista de Tareas y Progreso: PSTorrent5

Fork de trabajo sobre [jfcardososantos/PS5Torrent](https://github.com/jfcardososantos/PS5Torrent) v1.0.0 (GPL-3.0), renombrado a **PSTorrent5**.
Local: `J:\Codigo\pstorrent5` · Title ID: **`PPSA91289`** · Versión: **1.1.0** · Panel: `http://<IP_PS5>:12389/`
Referencia técnica completa: `ARCHITECTURE.md`.

---

## Hito 0: Base y validación en consola

- [x] Clonar el repositorio upstream en `J:\Codigo\pstorrent5`
- [x] Crear `ARCHITECTURE.md` y `TODO.md` desde las plantillas
- [x] Subir el SDK a **v0.44** en `setup.sh` (versión + SHA256) y en `.github/workflows/build.yml`, y actualizar `README.md`
- [x] Crear el fork en GitHub y reconfigurar remotos (`origin` = fork `LiMPbIzK/PSTorrent5`, `upstream` = `jfcardososantos/PS5Torrent`)
- [x] **Build local en Windows** con llvm-mingw y `build.ps1` (sin WSL, sin sudo, sin make) → genera `pstorrent5.elf`
- [x] `build.ps1` lee `SRCS`/`CFLAGS`/`INCLUDES`/`LDLIBS` del `Makefile` (sin divergencia con el build de CI/Linux)
- [x] Dejar de rastrear el ELF de build (sigue ignorado por `.gitignore`). Copia del binario del autor preservada en `.deps/reference/` para análisis
- [x] **GitHub Actions habilitado** en el fork y **CI en verde**: runs #1, #2 y #3 con `Build` + `Inspect payload` + `Upload ELF artifact` correctos (≈25 s por build)
- [x] Verificar que la CI genera `pstorrent5.elf` sin errores (artefacto `pstorrent5-elf` descargable)
- [x] Cargar el ELF en la consola (loader 9021): **carga sin problemas, el SIGILL del issue #1 está resuelto** con el SDK v0.44
- [x] Abrir el panel desde otro dispositivo de la LAN y añadir un `.torrent` (aceptado correctamente)
- [ ] **Bug encontrado y corregido**: tras añadir el torrent el panel dejaba de responder. Causa: el announce corre en el bucle principal y el `connect()` a cada peer era bloqueante **sin timeout** (se aplicaba después de conectar) → hasta ~75 s de congelación por peer inalcanzable. Corregido con connect no bloqueante acotado + presupuesto de 4 conexiones por announce. **Pendiente de validar en consola con el ELF nuevo**
- [ ] **Corregir `get_local_ip()`**: en el log aparece `http://6.4.6.0:12389/`, una IP que no existe. Solo afecta al aviso de la notificación y al banner (el panel funciona por la IP real), pero induce a error
- [x] **Motor de descarga verificado de extremo a extremo**: tracker HTTPS (TLS de la consola, aceptando la CA que el firmware no conoce) → peers → handshake → `unchoke` → bloques de 16 KB → ensamblado de pieza → **SHA-1 correcto y escritura en `/mnt/usb0`** (`piece 1/23844 ok (0.3 MB downloaded)`)
- [x] **Diagnóstico del enjambre (16-10-2026): el tracker de Ubuntu entrega UN peer por announce.** Verificado con `tools/` (sonda externa en Node, no en la consola): `torrent.ubuntu.com` devuelve siempre 65 bytes = 1 peer con `numwant` 50/200/1000, con `compact=0` y `compact=1`, por HTTP y HTTPS y también en `ipv6.torrent.ubuntu.com`, mientras informa de **415 seeders y 25 leechers**. **No es un bug del announce del cliente** (mismo resultado desde un PC con una petición escrita a mano y el `info_hash` real: `43519d14444904f6015e1a0d92018068fb0a49fa`). Con enjambre de 1 peer y encima leecher, decenas de KB/s es lo máximo que se puede sacar por esa vía.
- [x] **Rendimiento y protocolo (no bloqueante)**: `keep-alive` cada 120 s (sin él un peer nos cierra por silencio; se perdieron 2 peers por esto), drenado de **todos** los mensajes disponibles por peer y vuelta (antes 1 por vuelta, ataba la descarga a la cadencia del bucle), ventana de peticiones 5 → 16 bloques, reintento de announce adaptativo (30 s sin peers / 120 s con menos de 4, en vez de los 1800 s del tracker) y hasta 8 conexiones nuevas por announce (antes 2)
- [ ] Verificación pendiente: descarga **completa** de un torrent real, estado en el panel y escritura en `/mnt/usb0`
- [x] **Log a fichero implementado**: `/data/pstorrent5/pstorrent5.log` (reserva en `/data` y `/tmp`), rotación a 256 KB, volcado sin buffer para que sobreviva a una caída, visible en **`GET /api/log`** y en la ruta que muestra el panel. Se añaden trazas al motor (arranque del torrent, resultado del announce y errores)
- [ ] Actualizar `README.md`/`CHANGELOG.md` del fork con el nuevo SDK y la matriz de firmwares
- [x] **Cierre limpio e iteración rápida**: `POST /api/quit` cierra el payload desde dentro y `tools/deploy.ps1 -PS5Host <ip> -Build` hace el ciclo completo (compilar → parar el anterior → esperar a que libere el puerto → subir el ELF → verificar el panel). Solo `POST`
- [ ] **Regenerar el arte del tile**: `pkg/sce_sys/icon0.png`, `pic0.png` y `pic1.png` son los del proyecto original y llevan el nombre "PS5Torrent" dibujado. `pkg/generate_icon.py` ya escribe "PSTorrent5", así que basta regenerarlos (requiere Pillow) o retocar el arte

**Hito 0.5 (spike crítico, medio día) — decide la arquitectura**

- [ ] Medir velocidad de escritura sostenida como **ELF por loader** (USB y disco interno)
- [ ] Medir igual desde **folder app** de ShadowMount+
- [ ] Medir igual desde **fPKG**
- [ ] Confirmar/descartar el límite de ~2 MB/s del sandbox y decidir si el tile y el engine van siempre separados

**Notas técnicas del hito**
- El repo upstream tiene un commit que **solo añade un binario** (`4fb7830`, ELF de "v2.0.0 con RAR/7z + file selection") **sin código fuente**. No se toca; el `VERSION` del código sigue en `1.0.0`.
- `gh` no está instalado en el entorno: el fork debe crearse a mano en GitHub o instalando `gh`.
- **SHA256 del SDK v0.44** (verificado con dos herramientas): `4d06eab777bb135705b5a6816688c9132efb246093fe606ac488ab6cf65ca5b0`.
- El SDK v0.44 incluye toolchain **nativa de Windows** (`win\prospero-clang.cmd`, `prospero-clang++.cmd`, `prospero-lld.exe`, `ninja.exe`) que invoca `clang` del PATH con `-target x86_64-sie-ps5`. Alternativa al CI, pero **no hay `make` en Windows** (haría falta un `build.ps1`).
- Entorno local: WSL2 con **Ubuntu 24.04.4**, con `make`, `gcc` y `python3`, **sin clang-18/lld-18**. `sudo` pide contraseña.
- `prospero-llvm-config` exige `llvm-config-18` (paquete `llvm-18-dev`) para el build.
- **Toolchain de Windows resuelta sin sudo ni WSL**: `J:\llvm-mingw` (clang 23.1.3, portable, sin admin) + `build.ps1`.
- **LLVM 23 trae toolchain nativa de PS5**: con `-target x86_64-sie-ps5` y `SCE_PROSPERO_SDK_DIR` apunta él mismo, clang añade los crt (`crt1.o/crti/crtbegin/crtend/crtn`), el script `main.script` y las libs stub (`-lc_stub_weak -lkernel_stub_weak`). Por eso **no** hay que pasar `crt1.o` ni `-T ldscripts/elf_x86_64.x` (eso es de la ruta antigua, clang < 20).
- **clang ignora `--ld-path` y exige un ejecutable llamado `prospero-lld`**, mientras que lld elige driver por `argv[0]` (renombrar un `ld.lld.exe` a `prospero-lld.exe` da "lld is a generic driver"). Solución: `tools/prospero-lld/prospero-lld.c`, un shim que hace `_execvp("ld.lld.exe", argv)` con `argv[0]` correcto. Se compila a `.deps/bin/prospero-lld.exe`.
- Las libs SCE del SDK son **`.so`**, no `.a` (`libSceSystemService.so`, `libSceUserService.so`, `libkernel_web.so`…); `-lSceX` resuelve bien vía `-L target/lib`.
- **Validación del build local**: ELF64, `Type DYN`, máquina x86-64, `.text` en `0x4000` y secciones alineadas a 16 KB (página de PS5). Mismo tamaño (201.328 bytes) que el binario publicado por el autor, aunque con hash distinto (distinta versión de compilador).

---

## Hito 1: Configuración persistente, rutas y reanudación

- [x] **E/S de red totalmente no bloqueante**: el `connect` y el handshake de cada peer van por pasos (`peer_state_t`: IDLE/CONNECTING/HANDSHAKING/READY), con hasta 32 conexiones en vuelo y 16 candidatos lanzados cada 2 s, en vez de 4 cada 5 s. Ya no congela el panel. **Concesión documentada**: una vez READY, el cuerpo de cada mensaje se lee en modo bloqueante con `SO_RCVTIMEO` de 8 s, siempre con `select()` por delante (un mensaje legítimo llega entero)
- [ ] Reducir el `gethostbyname()` bloqueante del resolver (caché + límite, o resolver asíncrono)
- [ ] `src/config/`: config persistente en el directorio de datos del payload (JSON)
- [ ] Ajustes: ruta de descarga por defecto, extraer automáticamente, borrar comprimido, contraseñas por torrent, autenticación del panel
- [ ] Validación de rutas libres (absoluta, sin `..`, sin caracteres de control) y creación de `<destino>/torrents/`
- [ ] `GET /api/config` y `POST /api/config`
- [ ] UI de ajustes en el panel + explorador de carpetas
- [ ] **Reanudación:** persistir piezas completadas + verificación parcial al reabrir un torrent
- [ ] Detección de espacio libre en el destino (y vía `GET /api/v1/storage` de ShadowMount+ si está disponible)

---

## Hito 2: Extracción automática (libarchive)

- [ ] Cablear `scripts/setup_archive_deps.sh` en `setup.sh` y enlazar `-larchive -lz -llzma -lbz2` (+ `zstd`) en el `Makefile`
- [ ] `src/extract/extractor.h`: interfaz común (`start` / `step` / `status` / `cancel`)
- [ ] `src/extract/extract_queue.c`: cola FIFO, un trabajo activo, marcador `.extracted`
- [ ] `src/extract/extract_libarchive.c`: ZIP (incl. cifrado), 7z y RAR sin cifrar
- [ ] Extracción **incremental** por presupuesto de bytes en `torrent_mgr_tick()` (nunca bloqueante)
- [ ] Estado en `/api/torrents` (`extract_state`, `extract_progress`, `extract_error`) + barra en el panel
- [ ] Opción de borrar el comprimido tras extraer + comprobación de espacio libre previa

---

## Hito 3: Extracción con contraseña (7-Zip portado)

- [ ] **Spike:** compilar los códecs 7z+zip de 7-Zip con `ST_MODE=1` y extraer un archivo cifrado en la consola
- [ ] `makefile.machine` propio para PS5 (target FreeBSD, sin ASM x86, `ST_MODE=1`, sin CLI)
- [ ] `src/extract/sevenzip_backend.cpp`: API `IInArchive` + `CreateObject`
- [ ] `src/extract/extract_stream.cpp`: `ISequentialOutStream` → `open()/write()/mkdir()`
- [ ] `src/extract/extract_password.cpp`: `ICryptoGetTextPassword` desde config/UI
- [ ] Campo de contraseña por torrent en el panel
- [ ] Aislar el código RAR (licencia UnRAR incompatible con GPL-3.0) tras flag/módulo opcional y documentarlo

---

## Hito 4: Magnet completo

- [ ] **4a** BEP-10 (extension handshake) + BEP-9 (`ut_metadata`) usando los trackers `tr=` del magnet
- [ ] Persistir el `.torrent` obtenido en disco (habilita reanudación del magnet)
- [x] **4d** Trackers **HTTPS** resueltos con `libSceHttp2`/`libSceSsl` (pila TLS de la consola): `src/net_http.c` hace el GET y devuelve el cuerpo ya sin cabeceras, con timeouts, redirecciones y gzip. **Pendiente de verificar en consola la unidad de los timeouts** (la API sceHttp los quiere en microsegundos; se usan 6 s)
- [x] **Descubrimiento de peers por PEX (BEP-10 + BEP-11 `ut_pex`)**: era **la** solución al caso real medido (el tracker da 1 peer pero el enjambre tiene 415: el peer conectado conoce a los demás). Implementado en `peer_wire.c` (`peer_send_extended_handshake`, `peer_send_ut_pex`, recepción del mensaje id 20 con sub-id) y en `torrent_mgr.c` (`peer_handle_extended`, `peer_pool`). El handshake normal ya anunciaba el bit extendido en los reservados, pero **nunca se enviaba el handshake extendido**, así que ningún peer nos hablaba de PEX
- [x] **Medido contra peers reales (sonda externa en un PC, ver `ARCHITECTURE.md` §8): el tracker está clavado a UN peer y ese peer no soporta extensión, así que PEX no puede funcionar con este torrent.** 8 announces con `peer_id`, puerto y `numwant` distintos devuelven siempre el mismo `185.125.190.59:6908` (o ninguno) con 293 seeders en el enjambre: es un tracker "de puntero" que reparte un solo peer y confía en PEX/DHT para el resto. Y a ese peer le llegan los reservados a cero: **sin BEP-10 no hay PEX posible**. Conclusión: para este caso la vía es **DHT**
- [x] **Bug propio corregido (era la causa de las desconexiones instantáneas)**: mandábamos el handshake extendido a todos los peers. A uno que no lo ha anunciado, un mensaje de extensión (id 20) le parece una violación de protocolo y cierra. Reproducido: con `interested` el peer aguanta; añadiendo el mensaje de extensión, cierra en 0,14 s. Ahora se manda **solo** si el peer puso el bit `0x10` en `reserved[5]`
- [ ] **Validar en consola** que ya no hay desconexiones instantáneas y que en el log aparece la marca de build y —cuando el peer sí soporte extensión— `supports ut_pex (id N)` / `ut_pex: N new peer(s)`
- [x] **Descargar de varios peers a la vez**: hasta 4 piezas en curso (`inflight_t`), cada una servida por un peer distinto y con su propia ventana de peticiones. El buffer de cada ranura se reserva al usarse y el número de ranuras se ajusta al tamaño de pieza (4 con 256 KB, 1 con 16 MB) para no reservar decenas de MB. Una ranura muda 120 s se suelta para que la coja otro
- [ ] **Contar bloques en vez de bytes** en la pieza en curso: hoy un bloque duplicado inflaría el contador y forzaría un reintento (el SHA-1 lo detecta, pero es trabajo perdido)
- [ ] **Embeber un bundle de CAs** y validar de verdad la cadena TLS: hoy `sceHttp2SetSslCallback` la acepta sin verificar porque la tienda del firmware no conoce la jerarquía nueva de Let's Encrypt ("Root YR" → `0x8095F00C`, `SCE_SSL_ERROR_UNKNOWN_CA`)
- [x] **4b** Trackers **UDP** (BEP-15) implementados: `announce_udp()` en `tracker.c` (connect + announce, timeouts de 3 s por peticion) y `udp://` aceptado en la lista. **Medido desde la red del usuario**: `tracker.opentrackr.org:1337` → 30 peers, `open.demonii.com:1337` → 16, `tracker.torrent.eu.org:451` → 11, para el mismo torrent en el que el tracker de Ubuntu da uno. `exodus.desync.com` y `tracker.openbittorrent.com` no contestan desde aquí y no se incluyen
- [x] **Lista extra de trackers** (`tracker_extra_list()` en `tracker.c`): los tres UDP que respondieron se prueban en **todos** los torrents, ademas de los suyos. **Ojo**: se anuncian a trackers publicos tambien los torrents de tracker privado (no hay flag `private` en el parser); si molesta, vaciar la lista. Y el cursor por rondas (`announce_cursor`) es imprescindible: sin el, los dos primeros trackers que contestan bien acaparan todas las rondas y los extra no se probarian nunca
- [x] **4c** **DHT** (BEP-5 + KRPC) implementada (`src/dht.c`): tabla de 256 nodos, `wants` por info_hash, bootstrap contra 4 routers, `get_peers`/`announce_peer`, autoprueba del formato al arrancar y estado en el log cada minuto. **Limitacion medida en esta red: los routers DHT no contestan** (con NTP, DNS y los trackers UDP funcionando, o sea que el UDP no esta bloqueado en general: o son esos routers o la red filtra DHT en concreto). El codigo se queda porque en otras redes sí sirve; en esta, la fuente de peers son los trackers UDP

---

## Hito 5: Panel web y API

- [x] **Internacionalización del panel web** (`src/web/index.html`): tabla `I18N` con **es/en/pt**, detección automática (`?lang=` → localStorage → navegador), selector en la cabecera y `tools/check-i18n.js` verificado en CI
- [x] **Códigos de error estables** (`error_code`) en la API y en el estado de cada torrent; los mensajes del servidor pasan a inglés (klog y reserva) y el panel los traduce
- [x] Otras dos bombas de pila eliminadas en el motor: `uint8_t piece_buf[256 KB]` en `process_peer()` (ahora `static`) y `char data_b64[1 MB]` en la subida de `.torrent`
- [ ] Autenticación (token o Basic) con aviso en el panel
- [ ] Gestión completa: pause / resume / delete / prioridad / reintentar
- [ ] Selección de ficheros del `.torrent`
- [ ] Historial de descargas y log visible en el panel
- [ ] Sección de ajustes con estado de SM+, del loader 9021 y del espacio libre

---

## Hito 6: Folder app, auto-actualización y ShadowMount+

- [ ] Estructura del folder app: `eboot.bin`, `sce_module/libc.prx`, `sce_sys/` (param.json, icon0.png, pic0/pic1 `.dds` BC7, snd0.at9), `engine/pstorrent5.elf`
- [ ] Title ID `PPSA91289` en `param.json` y `origin-param.json` (`.pkg`, folder app y engine)
- [ ] ZIP de release con `<TITLE_ID>/` top-level y **todos los ficheros a 0777**
- [ ] Reutilizar el tooling del boilerplate (conversor a **FSELF** + `libc.prx` shim limpio) para el `eboot.bin` del tile
- [ ] Manifiesto propio: generación de `manifest.json`, firma **Ed25519**, contador anti-replay
- [ ] Cliente de update en la app + **helper** que se envía al loader 9021 y hace el swap por `rename()`
- [ ] Backup de la versión anterior en `previous/PPSA91289-backup`
- [ ] Arranque/parada del engine por el loader desde el tile (+ endpoint de reinicio del motor)
- [ ] Staging `<unidad>/.pstorrent5-staging/<hash>/` sin `sce_sys/` ni extensión de imagen
- [ ] Publicación por `POST /api/v1/manual/add` + `POST /api/v1/scan`, con degradación a `rename()` si no hay SM+
- [ ] Notificaciones (inicio, descarga completa, extracción completa, título publicado, actualización aplicada)

---

## Hito 7: Release y documentación

- [ ] CI por tag: ELF + ZIP del folder app + `.pkg` + `SHA256SUMS` + manifiesto firmado
- [ ] README del fork: instalación (los 3 métodos), matriz de firmwares, avisos de jailbreak y uso responsable
- [ ] `THIRD_PARTY_NOTICES` (boilerplate, ShadowMount+, libarchive, 7-Zip/UnRAR, miniz)
- [ ] Guía de resolución de problemas: loader ausente, SM+ ausente, app instalada como imagen, permisos 0777

---

## Decisiones cerradas (no reabrir sin motivo)

| Tema | Decisión |
|---|---|
| Base | Fork de `jfcardososantos/PS5Torrent` (GPL-3.0) |
| SDK | v0.44 |
| Title ID | `PPSA91289` para los tres artefactos |
| Magnets | BEP-10/9 → UDP → DHT → HTTPS |
| Extracción | libarchive primero, 7-Zip después para contraseñas |
| HTTPS | Pila de la consola (`libSceHttp`/`libSceSsl`), sin mbedTLS |
| Entrega | `.elf` + `.pkg` + folder app con auto-update |
| Auto-update | Manifiesto propio firmado con Ed25519, helper por el loader 9021 |
| Publicación ante SM+ | `manual.lst` vía API (`manual/add` + `scan`) |
| Staging | Oculto en la misma unidad que el destino, `rename()` atómico |
| Build | GitHub Actions (ELF/ZIP) + Windows local (.pkg) |
