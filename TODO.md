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
- [ ] Verificación pendiente: descarga completa de un torrent real, estado en el panel y escritura en `/mnt/usb0`
- [x] **Log a fichero implementado**: `/data/pstorrent5/pstorrent5.log` (reserva en `/data` y `/tmp`), rotación a 256 KB, volcado sin buffer para que sobreviva a una caída, visible en **`GET /api/log`** y en la ruta que muestra el panel. Se añaden trazas al motor (arranque del torrent, resultado del announce y errores)
- [ ] Actualizar `README.md`/`CHANGELOG.md` del fork con el nuevo SDK y la matriz de firmwares
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

- [ ] **E/S de red totalmente no bloqueante** (obligatorio): integrar los sockets de peers y de tracker en el `select()` del bucle principal, con estado por conexión, en lugar de `send`/`recv` bloqueantes. Elimina la congelación del panel de raíz (hoy mitigada con timeouts acotados)
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
- [ ] **4b** Trackers **UDP** (BEP-15)
- [ ] **4c** **DHT** (BEP-5 + KRPC) para magnet sin trackers
- [ ] **4d** Trackers **HTTPS** mediante `libSceHttp`/`libSceSsl` (sin portar TLS)

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
