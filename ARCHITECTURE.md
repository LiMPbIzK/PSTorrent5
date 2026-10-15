# 🏛️ Arquitectura del Proyecto: PS5Torrent (fork)

## 1. Reglas Generales y Filosofía

* **Objetivo:** Cliente BitTorrent para PlayStation 5 con jailbreak (FW ≤ 13.60), con panel web accesible desde la LAN, ruta de descarga configurable, extracción automática de ZIP/7Z/RAR (incluidas contraseñas) y auto-actualización sin reinstalar PKG ni enviar ELF a mano.

* **Origen:** fork de [jfcardososantos/PS5Torrent](https://github.com/jfcardososantos/PS5Torrent) v1.0.0 (C puro, motor BitTorrent propio, sin libtorrent).

* **Principios de Diseño:**
  * **Cero dependencias innecesarias.** Cada librería nueva debe justificar su tamaño y su coste de port.
  * **Nada bloquea el bucle principal.** Todo el trabajo pesado (red, hash, extracción) es incremental por presupuesto de bytes por tick.
  * **Entrega por capas.** `.elf` (motor), `.pkg` (instalación clásica) y folder app de ShadowMount+ (tile + auto-update).
  * **Incógnitas primero.** Los riesgos se verifican en consola en el día 1, no en la semana 6.
  * **Seguridad de datos del usuario:** nunca exponer contenido incompleto a terceros (ShadowMount+) ni sustituir ficheros de forma no atómica.

* **Alcance (sí):** descarga BitTorrent completa (trackers HTTP/UDP/HTTPS, DHT, magnet BEP-9/10), panel web en LAN, extracción automática, auto-actualización, instalación por PKG/ELF/folder app.

* **Fuera de alcance:** streaming de vídeo durante la descarga, cliente remoto fuera de la LAN, seeding agresivo con ratio/cola, migración a libtorrent/Transmission (requeriría libcxx + Boost para PS5, coste desproporcionado), watch-folder, transcodificación.

## 2. Stack Tecnológico y Dependencias Core

* **Lenguaje / Framework:**
  * **C (C11)** para el motor: cliente BitTorrent, servidor HTTP propio, panel web embebido, extracción, configuración.
  * **C++ puntual y aislado** para el port de 7-Zip (fase de contraseñas) y para el tile app si se reutiliza el boilerplate.
  * **HTML/CSS/JS embebido** en el payload (`src/web/index.html` → `src/web_content.h` por `scripts/embed_web.py`).

* **Toolchain y compilación:**
  * **ps5-payload-sdk v0.44** (añade soporte de FW 13.60; v0.41 causaba SIGILL). LLVM 18 (`clang`/`lld`), target FreeBSD.
  * **libc++ estático ya incluido en el ZIP del SDK** (su CI ejecuta `./libcxx.sh` antes de empaquetar) → C++ disponible sin construir toolchain.
  * **Build remoto** con GitHub Actions (`ubuntu-24.04`) para el ELF y los ZIP; **.pkg local** en Windows con .NET + LibProsperoPKG.
  * **Prohibido compilar en Windows nativo**: `setup.sh` es macOS/Linux. Usar CI o WSL2.

* **Librerías Principales:**
  * **libarchive 3.7.4** (fase 2a, ya preparada en `scripts/setup_archive_deps.sh`): ZIP (incl. cifrado), 7z y RAR sin cifrar. Requiere zlib + liblzma + bzip2 (+ zstd si se activa). **No soporta 7z ni RAR cifrados.**
  * **7-Zip (código `Alone2/makefile.gcc`, `ST_MODE=1`)** (fase 2b): cubre contraseñas, cabeceras cifradas y multi-volumen. Se enlaza **solo la capa de códecs + `UI/Common` mínimo** vía `IInArchive`, con `ISequentialOutStream` propio e `ICryptoGetTextPassword`. **No compilar la CLI** (evita `locale`/`argv`/consola).
  * **Pila TLS de la consola**: stubs del SDK `libSceHttp`, `libSceHttp2`, `libSceSsl`, `libSceNet`. **No se porta mbedTLS/OpenSSL.** Sirve para el auto-updater y para los trackers HTTPS.
  * **miniz** (solo si se reutiliza el kit de self-update del boilerplate) para validar/desempaquetar el ZIP de actualización.
  * **libSceAppInstUtil** (stub disponible) reservado para un futuro auto-update nativo del `.pkg`.
  * Enlazado actual del motor: `-lufs -lSceSystemService -lSceUserService`.

* **Licencias y obligaciones:**
  * El fork es **GPL-3.0** y debe seguir siéndolo; se conservan avisos de copyright y se mantiene `THIRD_PARTY_NOTICES`.
  * Código reutilizado de `blackbearreloaded/ps5-native-app-boilerplate` y `drakmor/shadowMountPlus`: **GPL-3.0-or-later** (compatible).
  * **7-Zip**: LGPL-2.1-or-later (compatible), **pero su parte RAR usa licencia UnRAR, incompatible con GPL-3.0** → el soporte RAR va tras un flag/módulo opcional y documentado.

## 3. Estructura de Directorios

```
pstorrent5/
├── .github/workflows/        # CI: ELF, ZIP del folder app, release con SHA256SUMS y firma
├── include/                  # headers C del motor
│   ├── bencode.h  sha1.h  torrent.h  tracker.h  peer_wire.h  piece_mgr.h
│   ├── file_io.h  net_utils.h  http_server.h  ui.h
│   ├── storage_paths.h  ps5_jailbreak.h  ps5_browser.h
│   └── (nuevos) config.h  extractor.h  magnet.h  shadowmount.h  version.h
├── src/
│   ├── main.c                # bucle principal single-thread cooperativo
│   ├── bencode.c  sha1.c  torrent.c  tracker.c  peer_wire.c  piece_mgr.c
│   ├── file_io.c  net_utils.c  ui.c  ps5_jailbreak.c  ps5_browser.c
│   ├── http_server.c         # servidor HTTP propio + rutas /api/*
│   ├── torrent_mgr.c         # gestor multi-torrent
│   ├── storage_paths.c       # rutas conocidas (hoy hardcodeadas)
│   ├── config/               # (nuevo) config persistente JSON
│   ├── extract/              # (nuevo) cola + backends de extracción
│   ├── magnet/               # (nuevo) BEP-10, BEP-9, UDP tracker, DHT
│   ├── net/                  # (nuevo) transporte HTTPS por libSceHttp
│   ├── shadowmount/          # (nuevo) staging + publicación por API de ShadowMount+
│   ├── update/               # (nuevo) update check + cliente del helper
│   └── web/index.html        # fuente editable del panel (→ web_content.h)
├── pkg/                      # metadatos del fPKG (sce_sys/param.json, iconos, TILE.md)
├── tile/                     # (nuevo) folder app: eboot.bin, sce_module/libc.prx, sce_sys/, engine/
├── scripts/                  # setup, deps de extracción, embed del panel, diagnóstico
├── tools/                    # empaquetadores (LibProsperoPKG), utilidades de release
├── Makefile                  # build del ELF del motor
├── setup.sh                  # preparación reproducible del entorno
├── ARCHITECTURE.md           # este documento
└── TODO.md                   # estado y progreso
```

## 4. Convenciones de Código y Restricciones Técnicas

* **Concurrencia:** un único hilo. **Prohibido bloquear** el bucle (`http_server_poll()` + `torrent_mgr_tick()` + `usleep`). La extracción, el hash y la descarga avanzan por *presupuesto* (KB/MB por tick). No se comparte estado entre hilos.
* **E/S de disco:** solo `open()/write()/rename()` POSIX (sin libufs, como el upstream). **Toda publicación de ficheros debe ser atómica** (escribir `.new`/`.upload` + `rename()` en la misma unidad).
* **Memoria:** buffers estáticos o de tamaño acotado por torrent; nada de reservar por el tamaño total del archivo comprimido (streaming).
* **Rutas:** toda ruta de usuario se valida (absoluta, sin `..`, sin caracteres de control) y se sanea antes de usarse. Las descargas van siempre a `<config:destino>`, nunca a rutas sueltas.
* **Identidad de la app:** title ID único **`PPSA91289`** (`contentId: IV9999-PPSA91289_00-PS5TORRENT000001`) para `.elf`, `.pkg` y folder app. El formato es `^[A-Z]{4}[0-9]{5}$` (lo valida LibProsperoPKG) y la banda `PPSA9xxxx` es la de homebrew (no colisiona con juegos).
* **Modos de fichero:** todo fichero del folder app y de su ZIP debe quedar en **modo 0777** (si no, la consola no arranca la app).
* **Primitivas de red:** los trackers HTTP usan el cliente propio; HTTPS usa la pila de la consola. El cliente HTTP debe ser tolerante a respuestas parciales y nunca bloquear más de un tick.
* **Errores:** todo fallo visible en el panel con causa concreta (no mensajes genéricos), y toda operación destructiva precedida de verificación (espacio libre, hash, título).
* **Estilo:** coincidir con el código existente: C estilo K&R, identificadores en `snake_case`, prefijos por módulo (`torrent_`, `tracker_`, `storage_`), cabeceras con guardas `PS5TORRENT_*_H` y comentarios de bloque `/** ... */` en las APIs públicas.
* **Pruebas:** no hay framework nativo; cada módulo nuevo debe poder probarse en el host (compilación nativa de la parte pura: parser, cola, sanitizado de rutas) además de en consola.

## 5. Identidad y Forma de Entrega

| Artefacto | Instalación | Auto-update |
|---|---|---|
| `ps5_torrent.elf` | Enviado al loader (puerto 9021) | Manual |
| `PS5Torrent.pkg` | Instalador de fPKG | Manual |
| `<TITLE_ID>.zip` (folder app) | Copiado a `/data/homebrew/PPSA91289/` → tile en el menú (ShadowMount+) | **Automático** |

**Arquitectura de dos piezas (obligada):** un app registrado corre en sandbox, **no puede escribir en su propia carpeta** y la consola **limita sus escrituras a ~2 MB/s** pasados unos cientos de MB. Por eso:

* **`ps5_torrent.elf` (engine):** motor + panel web + extracción, lanzado por el loader. Sin throttling de escritura, acceso total al FS.
* **Tile app `PPSA91289`:** icono en el menú, abre el panel, arranca el engine por el loader si no está, y gestiona la auto-actualización.
* El release ZIP incluye el engine dentro (`PPSA91289/engine/ps5_torrent.elf`), de modo que el helper actualiza **tile y motor a la vez**.
* **Nunca instalar a la vez** el folder app y el `.pkg` con el mismo title ID.

## 6. Auto-actualización

* **Modelo de publicación:** **manifiesto propio** con **clave Ed25519** propia; `manifest.json` + `manifest.sig` servidos junto al repo (GitHub Pages o URL directa). Contador anti-replay en `/download0/self-update-sequence` (equivalente).
* **Flujo (dos programas):** la app verifica el manifiesto firmado y compara versiones → pregunta al usuario → **envía el helper al loader 9021** y reutiliza esa conexión → descarga el ZIP por HTTPS reenviándolo al helper → el helper revalida tamaño y SHA-256, valida el archivo (sin rutas absolutas/`..`, sin enlaces, sin ficheros especiales, sin cifrado, sin duplicados, una sola app) y lo descomprime al lado → `apply` → **la app se cierra sola** y el helper hace el swap por `rename()` y notifica.
* **Backup:** los ficheros reemplazados se conservan en `<data-dir>/previous/PPSA91289-backup` (solo la última versión).
* **Publicación de una release:** subir `contentVersion` en `sce_sys/param.json` → tag → CI construye tile + engine, ZIP con `<TITLE_ID>/` top-level a 0777, `SHA256SUMS` y firma → se publica.
* **Degradación:** si el loader en 9021 no responde o la app está instalada como imagen, el update se rechaza con mensaje explícito y se ofrece el ZIP para actualización manual.

## 7. Integración con ShadowMount+

* **Detección:** SM+ solo considera montable (a) una carpeta con **`sce_sys/param.json` en su raíz**, o (b) un fichero **`.ffpkg` / `.exfat` / `.ffpfs` / `.ffpfsc`**. Todo lo demás es invisible.
* **El candado es no cumplir esas condiciones durante la descarga.** No se usan permisos ni bloqueos de fichero. La ventana `stability_wait_seconds=10` de SM+ es insuficiente (el `mtime` de la carpeta raíz no cambia al escribir ficheros dentro) y no es de fiar.
* **Staging:** `<unidad-destino>/.ps5torrent-staging/<info-hash>/` — misma unidad que el destino para que la publicación sea atómica. Sin `sce_sys/`, sin extensión de imagen, con marcador `.ps5torrent-incomplete`.
* **Publicación:** al verificar la descarga (SHA-1 de todas las piezas) y terminar la extracción: `rename()` a `/mnt/usbW/homebrew/PPSA91289/` (o `PPSA91289.ffpkg`) + `POST /api/v1/manual/add` + `POST /api/v1/scan`. Enlace (hardlink) simbólico no se usa.
* **API de SM+** en `127.0.0.1:10101` (`api_enabled=1` por defecto): `manual/add`, `manual/remove`, `scan`, `storage` (espacio libre), `settings`, `games/info`, `games/mount`, `games/unmount`. Alcanzable desde el sandbox (precedente: el boilerplate usa `127.0.0.1:9021` desde una app).
* **Degradación sin SM+:** si la API no responde, se publica solo con el `rename()` a un scan path y se avisa.
* **Aviso documentado al usuario:** con `scan_depth=2`, un extraído bajo `/mnt/usb0/torrents/Juego/sce_sys/` sí sería montado a medias; el staging lo evita siempre.

## 8. Motor BitTorrent: límites conocidos y hoja de ruta

* Limitaciones del upstream: magnet sin BEP-9 (aborta con `num_pieces == 0`), solo trackers HTTP, sin DHT/PEX/MSE, sin reanudación, sin selección de ficheros, sin autenticación en el panel.
* **Magnet:** dos capas ortogonales. **BEP-10 + BEP-9 son obligatorios** (protocolo de extensión + `ut_metadata`) para obtener la lista de ficheros; **DHT (BEP-5) y los trackers UDP (BEP-15) solo descubren peers**. Orden: BEP-10/9 → UDP → DHT → HTTPS. Al obtener el metadata hay que **persistir el `.torrent`** en disco (da reanudación del magnet).
* **Extracción:** libarchive para el caso base (fase 2a) y 7-Zip para contraseñas (fase 2b), detrás de una interfaz `extractor_t` común. Reglas: streaming, incremental, marcador `.extracted`, comprobación de espacio libre, borrado opcional del comprimido.
* **Reanudación:** persistir piezas completadas + verificación parcial al reabrir. Es un requisito de facto para torrents grandes, no un extra.
