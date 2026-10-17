# UFC-48: write windows coherentes

Estado: causa identificada en código y vista en el log de Windows; arreglo implementado y
probado con un test unitario de la lógica de merge. **No validado en juego** (no se ejecutó).
Defaults de release sin cambios: `UFC3.exe` sigue con `BB_WRITE_WINDOW=0 BB_WRITE_STEP=0`; la
prueba usa un segundo launcher, `UFC3-WRITE-WINDOWS-TEST.exe`.

Base: `ufc3-online` + UFC-47 (late-label guard, para no mezclar síntomas) + UFC-2 (el audit
`BB_WRITE_WINDOW_DIAG` que produjo el log y su documento).

## Causa

Una window abre la página entera (4 KiB) a la CPU sin readback, aunque tenga bytes que escribió
la GPU (`gpu_modified_ranges` = R: VRAM tiene el valor bueno, el guest está viejo).
`DecideWriteStep` sólo comprueba que el *primer* write esté a más de 16 bytes de R; con la
página abierta cualquier thread escribe cualquier byte, incluidos los de R.

1. **Cierre (`CloseWindows` → `NoteSteppedWrite` → `ProcessLateWrites`)**: las palabras que la
   CPU cambió y que tocan R se **descartaban** (`stepped_write_conflicts`) y quedaban en R. El
   guest tiene el valor de la CPU, R dice «GPU-owned».
2. **Siguiente readback de la página (`DownloadMemory` → `TryWriteBacking`)**: copia R desde
   VRAM al guest → **pisa la escritura de la CPU** con el valor viejo de VRAM.
3. Variantes del mismo hueco: un readback *durante* la window (o en vuelo mientras se cierra)
   escribe VRAM sobre bytes que la CPU acaba de escribir sin fault.

Si esos bytes son enlaces de la free list del allocator (memoria que la GPU escribió, se liberó
y el allocator reusa como nodos), el «next» queda con datos GPU → `mov rax,[r13]` en
`+0x1262a2a` con `r13=-1` (UFC-2).

### Evidencia en el log (UFC2-diag-logfolder-win-20261016_160516.log, Windows RTX 3060)

- Líneas 2795–2821: 24 `upload-skip` en la página `0x100077e000`, `opener=0x1262c9f`
  (el allocator, vecino del fault `0x1262a2a`), `caller=+0xf409b`. Los valores son una free
  list de nodos de 32 bytes: `0x...ec20 → 0x100077ec40`, `ec40 → 0x100077ec00`,
  `ec60 → 0x100077ec80`, `ee60 → 0x100077ecc0` (la mitad alta de cada puntero, `payload=0x10`,
  es la palabra siguiente). Son escrituras del allocator hechas por la window sobre bytes en R,
  **descartadas** al cerrar: el paso 1 de arriba, sobre la free list.
  El ticket los leía como «limpios» por `changed_since_snapshot=0`, pero ese campo compara con
  el snapshot *de cierre*, que ya contiene esas escrituras: no dice nada de si se subieron.
- El audit viejo **no podía ver** el paso 2 sobre esa página: `readback-overwrite` exigía
  `guest != snapshot de cierre`, y en el paso 2 el guest todavía es igual al snapshot (es la
  escritura de la CPU) cuando el readback la pisa.
- Los 64 `readback-overwrite` reportados son casi todos **falsos positivos**: 62 en
  `0x10013fd160+4` (un contador que escribe la GPU, `opener=0x12590c2`) forman una cadena exacta
  `guest(n) == payload(n-1)` (comprobado sobre las 62 líneas, 0 cortes): es un readback que pisa
  al readback anterior, no a la CPU. Los dos restantes (`id=3022` en `0x1000aff000`,
  `id=138221` en `0x1001338000`) son floats y no se pueden atribuir con ese audit. Además el tope
  de 64 se agotó con esa cadena (línea 2996).
- Contexto, no prueba: en la carga posterior a la pelea (`EntityLoader` creado en 2954–2956; stats en 2952–3192) los
  conflicts suben a 6881, 3993 y 27541 por ventana de 5 s y los FPS caen a 7,5 antes del freeze.

### Por qué Windows y no (o menos) Linux

**No determinado.** El camino de código es el mismo en las dos plataformas (no depende de
VEH/VirtualProtect frente a mprotect: el write se pierde por la regla de ownership, no por la
protección). En Linux las windows también estaban activas cuando se midió 137–196 FPS; sin un
log de Linux con el audit no se puede decir si allí los writes se pierden menos (timing,
reutilización de la memoria) o se pierden sin crashear. El arreglo es independiente de la
plataforma.

## Arreglo

Regla: cada submission cierra todas las windows antes de que la GPU la vea, así que toda GPU
write que una window pueda tocar se envió **antes** de que la window abriera. Un byte que la CPU
cambió en la window es el valor más nuevo: gana la CPU.

Por granos de **8 bytes alineados** (`WindowMerge::Granule`): un puntero cambiado en parte (la
mitad alta igual al valor viejo del guest) es todo de la CPU. Los bytes GPU que el juego tiene
junto a sus datos CPU están a más de 16 bytes (gate de `DecideWriteStep`).

| Archivo | Cambio |
| --- | --- |
| `gpu/shadps4/video_core/write_window_merge.h` (nuevo) | Lógica pura: `ForEachChangedRun` (diff de cierre por granos) y `ForEachWritableRun` (qué bytes de un readback se escriben) |
| `page_manager.cpp` `CloseWindows` | Todo el cierre bajo `windows_mutex` (antes soltaba el lock antes de reproteger y diffear: un fault podía abrir otra window encima, y un readback no veía ni la window ni su diff). Los granos cambiados van a `NoteSteppedWrite(..., from_window=true)` |
| `page_manager.cpp` `ReadbackSnapshot` / `WriteReadback` (nuevos) | El readback toma al empezar los bytes sobre los que puede escribir (en páginas con window, el `before` de la window); al escribir salta los granos que cambiaron desde entonces o desde que abrió la window. En una página con window abierta la pone read-only durante el merge (un write de la CPU en ese momento falla, espera `windows_mutex` y se reintenta) y actualiza el `before` con lo que escribe (no es un write de la CPU: el cierre no lo sube como tal) |
| `buffer_cache.cpp` `NoteSteppedWrite` | `from_window`: resta esos bytes de `gpu_modified_ranges` en el momento (todavía bajo el lock de la window) y los encola aparte (`window_writes`) |
| `buffer_cache.cpp` `ProcessLateWrites` | Los granos de una window se suben a VRAM enteros, toquen bytes GPU o no. El single-step (`stepped_writes`) conserva la regla anterior (descartar conflictos): no se cambió su comportamiento |
| `buffer_cache.cpp` `DownloadMemory` | Escribe a través de `PageManager::WriteReadback` con el snapshot de inicio (sólo si windows+step están activos y twins no) |
| Stats | `Write windows: N/s opened instead of a readback; CPU took over X GPU bytes, readbacks kept Y CPU bytes (UFC-48)` |
| Audit (`BB_WRITE_WINDOW_DIAG`) | Tope configurable `BB_WRITE_WINDOW_DIAG_MAX` (default 64). `readback-overwrite` redefinido: un readback (ya escrito) dejó el payload sobre bytes que la window cambió, y no lo que la window dejó; el snapshot sigue a los readbacks (se acaban las cadenas readback-sobre-readback) y los bytes que escribió un readback durante la window no cuentan como cambio de la CPU. Campos nuevos: `window_changed`, `overwritten`, `window_left` |
| `packaging/windows/sacredfire_launcher.c` | `#ifdef UFC3_WRITE_WINDOWS_TEST`: fuerza `BB_WRITE_STEP=1 BB_WRITE_WINDOW=1 BB_WRITE_WINDOW_DIAG=1 BB_WRITE_WINDOW_DIAG_MAX=1000`, título y línea de log propios. Sin el define (el `UFC3.exe` del zip) no cambia nada |
| `tests/test_write_window_merge.cpp` (`write-window-merge-test`) | Diff por granos, merge, y un modelo del caso del log: con la política anterior el enlace `0x100077ec40` termina en `0xffffffffffffffff`; con la nueva sobrevive al cierre, a un readback durante la window y a uno en vuelo a través del cierre |

### Límites conocidos

- Un write de la CPU que deja **exactamente** el valor viejo del guest en un grano entero no se
  ve en ningún diff: ese grano sigue en R y un readback lo pisa. Es indetectable sin fault; los
  granos de 8 bytes lo reducen (antes era por byte en la práctica) pero no lo eliminan.
- Una escritura de host al guest durante una window (labels del CP, readbacks de imágenes) se
  trata como de la CPU: pasa a VRAM y sale de R. El guest es la copia correcta en esos casos.
- Una GPU write grabada en el GPU thread *después* del cierre y antes de `ProcessLateWrites`
  sobre los mismos bytes queda ordenada por la cola (gana la que se ejecuta después), igual que
  antes para los bytes fuera de R. Sólo pasa si el juego escribe con la CPU memoria que una
  submission anterior todavía está escribiendo sin esperarla (una carrera del propio juego).
- El single-step conserva su regla de descartar conflictos (y su regresión conocida de pieles
  negras en peleadores creados, por eso la prueba es con peleadores de fábrica).

## Compilación

- Linux: `ufc3-build` → exit 0 (PGO off, LTO ON). `write-window-merge-test` y
  `label-guard-test` (UFC-47) pasan.
- Windows: `scripts/build-windows.sh --runtime` (toolchain/sysroot/deps/downloads enlazados del
  checkout principal, bajo el lock de `ufc3-build`) → exit 0, `ufc3-v0.1.4-25-gdfc7705`.
  `scripts/package-windows.sh` limpio. El launcher de prueba se compiló aparte con los mismos
  comandos de ninja más `-DUFC3_WRITE_WINDOWS_TEST`, se le quitó la info de debug y se agregó al
  zip como `UFC3-WRITE-WINDOWS-TEST.exe` junto con `README-UFC48.txt`.
- Paquete: `~/UFC3Native-windows-ufc48-dfc7705.zip` (+ `.sha256`).

## Una prueba

Paquete: `~/UFC3Native-windows-ufc48-<commit>.zip`. Abrir **`UFC3-WRITE-WINDOWS-TEST.exe`**
(no `UFC3.exe`); el título dice «UFC-48 write windows test». Misma corrida que UFC-2: Fight Now
con peleadores de fábrica → pelea completa → post-pelea → volver y cargar la segunda; hasta 20
min o el primer crash/freeze. Mandar el log completo.

Qué buscar en el log:
- Línea `UFC-48 write windows test: ...` y `Write window audit: on; ... at most 1000 reports`.
- `Write windows: N/s opened instead of a readback; CPU took over X GPU bytes, readbacks kept Y`
  → N > 0 confirma windows activas; X > 0 son los casos que antes se perdían (ahora se suben).
- `Write window audit: readback-overwrite` → **debe haber 0** (o casi: un caso legítimo es la
  GPU reescribiendo después bytes que la CPU cambió en una window; mandar cualquiera que salga).
- `Write window audit: upload-skip` → con windows ya no se descartan; si aparecen, son de
  single-steps (correlacionados por página con la window más reciente).
- `Stepped writes: ... conflicts` → ahora cuenta sólo los del single-step; no debe dispararse a
  miles en la carga de la segunda pelea como en el log anterior.
- `Frame stats: X FPS` durante la pelea frente al build actual (0.1.4 / UFC-47, windows
  apagadas). En el log de UFC-2 con windows la pelea iba a 109–192 FPS.
- Un fault en `+0x1262a2a` o un freeze con este build refuta el arreglo como suficiente: mandar
  el log (el audit imprime el `Write window audit fault` con r12–r15 y la cabeza del allocator).

## Segunda pasada: assert «write-only» al entrar a Career

Prueba en Windows (RTX 3060 laptop) con `UFC3-WRITE-WINDOWS-TEST.exe` de dfc7705
(`~/ufcCOntext/external-logs/UFC48-test-win-20261016_170301.log`): Quick Fight bien (sin
`readback-overwrite`/`upload-skip`, 0 conflictos), pero al cargar Career:
`page_manager.cpp:1179 Protect: Assertion Failed! Attempted to protect region as write-only` →
exit 23. Es un assert del port.

**Causa (regresión de 558ae55, no del single-step).** Con readbacks precisos (`BB_READBACKS=2`,
lo que pone el launcher) una página con bit GPU en el `RegionManager` tiene un read watcher, y
una con bit CPU no tiene write watcher; si tiene los dos, `PageState::Perms()` daba `Write`
solo. El invariante que lo evitaba: el bit GPU de una página implica bytes suyos en
`gpu_modified_ranges`, y toda readback que los baja desmarca la página (`DownloadMemory` →
`UnmarkRegionAsGpuModified`). 558ae55 lo rompió: el cierre de una window saca de
`gpu_modified_ranges` los bytes GPU que la CPU pisó (`NoteSteppedWrite(from_window)`) pero la
página queda con su bit GPU. Si el cierre se llevó todos los bytes GPU de la página:

1. la siguiente escritura de la CPU ahí hace fault; `InvalidateRegion` ve el bit GPU →
   `DecideWriteStep` cuenta 0 bytes GPU (ni step ni window) → `ReadMemory`;
2. `DownloadMemory` no encuentra nada que copiar y volvía **antes** de `UnmarkRegionAsGpuModified`;
3. el fault marca la página CPU-modified: read watcher sin write watcher → `Protect(Write)` →
   assert en Windows. En Linux (`userfaultfd`) el mismo estado deja la página sin acceso, y un
   read fault ahí volvía al mismo `DownloadMemory` vacío sin fin.

Con `BB_WRITE_STEP=1` solo (sin windows) no pasa: el step no saca bytes de
`gpu_modified_ranges`. Las otras restas (discard, twins, block moves, readback sin descarga)
desmarcan la página o mueven el bloque. No se reprodujo en Linux (no se corre el juego acá);
la cadena sale del código y del punto exacto del log (carga de Career, windows abiertas).

**Arreglo (9dcb459).**
- `DownloadMemory`: el retorno temprano sin bytes que bajar también desmarca el rango
  (`buffer_cache.cpp`). Corrige el estado: la página deja de ser GPU-modified y vuelve a
  `ReadWrite` por el camino normal.
- `PageState::Perms()` pasa por `PageWatch::FromWatchers` (`page_watch_perms.h`): sin lectura
  tampoco hay escritura. Es la semántica correcta (una página cuyos datos están sólo en VRAM no
  debe aceptar escrituras sin fault) y evita que otro estado inconsistente vuelva a pedir
  write-only. Test: `page-watch-perms-test` (`tests/test_page_watch_perms.cpp`).

**Compilación.** Linux `ufc3-build` exit 0 (`ufc3-v0.1.4-27-g9dcb459`). Windows
`scripts/build-windows.sh --runtime` exit 0, `scripts/package-windows.sh` limpio; launcher de
prueba recompilado con `-DUFC3_WRITE_WINDOWS_TEST` y agregado como antes. Paquete:
`~/UFC3Native-windows-ufc48-9dcb459.zip` (+ `.sha256`). `page-watch-perms-test` y
`write-window-merge-test` pasan.

**Una prueba.** `UFC3-WRITE-WINDOWS-TEST.exe`: entrar a Career y jugar la primera pelea de
Career completa; después Quick Fight con peleadores de fábrica, pelea completa. Mandar el log.
Esperado: sin el assert; mismas líneas `Write windows: ... CPU took over X GPU bytes` y 0
`readback-overwrite`. Sangre/sudor ausentes en golpes es UFC-49, no esto.

## Tercera pasada: host fault en el editor de luchador de Career (Windows)

Prueba 2 con `UFC3-WRITE-WINDOWS-TEST.exe` de 9dcb459
(`~/ufcCOntext/external-logs/UFC48-test2-win-20261016_172729.log`): el assert write-only ya no
aparece. Crash del host tras cambiar el pelo 4 veces en el editor de luchador:
`Host fault: read access to 00000000000001E9 at sacredfire-game.exe+0x13c123`, exit 3.

**Simbolizado** (`llvm-symbolizer` sobre `out/windows/bin/sacredfire-game.exe` de 9dcb459; su
`.text` es idéntica byte a byte a la del exe del zip, que sólo está sin debug info):
`+0x13c123` = `mov (%rax),%rax` del `operator++` del árbol de `std::set` (libc++ `__tree:175`),
dentro de `RangeSet::ForEachInRange` (`range_set.h:90`, el `it++`) ← `DownloadMemory`
(`buffer_cache.cpp:921`, `gpu_modified_ranges.ForEachInRange(address, size, add_download)`) ←
`MemoryTracker::ForEachDownloadRange` ← `ReadMemory`. No es `ReadbackSnapshot`/`WriteReadback`,
ni el audit, ni el step.

**Causa: carrera de datos sobre `gpu_modified_ranges`.** El iterador siguió el puntero de un
nodo liberado/reusado mientras lo recorría: `rax`/`rsi = 0x1e9` es basura leída como hijo
izquierdo. `rdi = 0x105382f000`, `r12 = 0x1053830000` son el `address`/`address+size` de la
página que bajaba (4 KiB exactos, dentro de la textura de pelo `0x105382b300`, 1 402 880 B, que
el juego llena con `memcpy` de ucrtbase: `343 into image 0x105382b300`, y 3362 write faults en
`0x1053975000`); no cruza de página. `rdx`/`r15` (`0xffffffefac7d1000`) son restas de esa
basura con los límites del rango, no un offset mal calculado.

El diseño suponía que el hilo GPU era el único que escribía `gpu_modified_ranges` (por eso lo
leía sin lock; los hilos con fault leían bajo `gpu_ranges_mutex` compartido). 558ae55 agregó
un escritor en otro hilo: `CloseWindows` corre en `Liverpool::SubmitGfx`/`SubmitAsc` (el hilo
del juego que hace el submit) y `NoteSteppedWrite(from_window)` hace `Subtract` ahí. El log lo
muestra a la vez: `Write windows: 300.8/s opened ...; CPU took over 14176 GPU bytes` y
`423.1 readbacks/s`. Un `Subtract` que rebalancea/libera nodos mientras el hilo GPU itera →
nodo liberado. (`NoteDiscardRange`, desde los discard locks del juego, ya era otro escritor
fuera del hilo GPU; mucho más raro.)

**Arreglo (bb4e938, `buffer_cache.cpp/.h`).** Toda lectura de `gpu_modified_ranges` en el hilo
GPU toma `gpu_ranges_mutex`: compartido en `ReadMemory` (skip-no-overlap y los caminos twins),
`ProcessLateWrites` (conflictos de steps), `ProcessDemotions`, `ProcessPendingAssets` y
`SynchronizeMemory` (nuevo `GpuRangesIntersect`); en `DownloadMemory` lectura + `Subtract` en
una sola sección exclusiva. El lock sigue siendo hoja: los rangos se copian antes de tomar
`keep_gpu_mutex`/`twins_mutex` (que `SynchronizeMemory` y el camino twins toman *antes* que
`gpu_ranges_mutex`), y `DiagnoseWindowWrite` corre ya liberado. Con lock hoja no hay deadlock
posible con `windows_mutex` (que `CloseWindows` tiene tomado al restar).

**Test.** No agregué uno: la carrera necesita el `BufferCache` entero con dos hilos; un stress
de `RangeSet` + `shared_mutex` sólo probaría la librería. `write-window-merge-test` pasa.

**Compilación.** Linux `ufc3-build` exit 0 (`ufc3-v0.1.4-29-gbb4e938`). Windows
`scripts/build-windows.sh --runtime` (bajo el lock de `ufc3-build`) exit 0,
`scripts/package-windows.sh` limpio; launcher de prueba recompilado con
`-DUFC3_WRITE_WINDOWS_TEST`, sin debug info, agregado con `README-UFC48.txt`. Paquete:
`~/UFC3Native-windows-ufc48-bb4e938.zip` (+ `.sha256`).

**Una prueba.** `UFC3-WRITE-WINDOWS-TEST.exe`: Career → editor de luchador → cambiar pelo y
otras partes varias veces (más de 4) → primera pelea de Career completa; después Quick Fight con
peleadores de fábrica, pelea completa. Mandar el log entero. Esperado: sin `Host fault`, sin el
assert; `Write windows: ... CPU took over X GPU bytes` y 0 `readback-overwrite`; FPS de pelea
como en la prueba anterior (el lock compartido es por readback, no por draw).

## Cuarta pasada: congelamiento en el editor y readback-overwrite que quedan

Prueba 3 con `UFC3-WRITE-WINDOWS-TEST.exe` de bb4e938
(`~/ufcCOntext/external-logs/UFC48-test3-win-20261016_174330.log`, 924 líneas): el host fault
de `+0x13c123` ya no aparece. El juego se **congeló** en el editor de Career (salir de la
categoría cabeza y volver a entrar): música sí, imagen e input no. El log termina en draws del
shader dither (pelo) y `open(/temp0/createplayer/cap_20002)`, sin STOP/fault/assert y sin
`GPU wait not met` (UFC-47). Además 7 `readback-overwrite` (líneas 732, 840, 842, 845, 863,
882, 885).

### Congelamiento

**El log no lo prueba** (ninguna línea después de que empieza; las stats salen del hilo GPU
cada 5 s y paran cuando el juego deja de enviar trabajo). Por código, la causa más probable es
un **fault reintentado para siempre**, no un deadlock entre los locks nuevos:

- `OpenWindow`, si la página ya está en `open_windows`, devolvía «abierta por otro hilo,
  reintentá» **sin tocar la protección**. Pero una página con window abierta puede volver a
  quedar protegida: un readback que la cubre le quita el bit GPU (`UnmarkRegionAsGpuModified`,
  también el retorno vacío nuevo de 9dcb459) y un binding escrito del hilo GPU se lo vuelve a
  poner (`ForEachUploadRange` → `ChangeRegionState<GPU>` → `UpdatePageWatchers` →
  `Protect(None)`). El siguiente write de la CPU hace fault, `DecideWriteStep` vuelve a pedir
  window (bytes GPU en la página, no cerca del write), `OpenWindow` la encuentra abierta y el
  write se reintenta sobre la página protegida: bucle infinito en el handler. El hilo del juego
  queda girando (imagen e input colgados), el audio es otro hilo, el GPU no recibe más trabajo
  (sin stats, sin `GPU wait`). Encaja con el síntoma; el editor tiene justo esa mezcla
  (~300 windows/s, ~450–1000 readbacks/s, bindings escritos cada frame en páginas con bytes de
  CPU al lado, p. ej. `0x10013fd000`).
- `ArmStep` tenía la misma forma: con la página ya en el slot no la volvía a abrir.
- Locks revisados (sin ciclo encontrado): `windows_mutex` → lock de página → `Protect`
  (`CloseWindows`, `OpenWindow`), `windows_mutex` → `gpu_ranges_mutex` (cierre), lock de región
  del tracker → `gpu_ranges_mutex` (`DownloadMemory`, `SynchronizeMemory`), `keep_gpu_mutex` →
  `gpu_ranges_mutex`. Nadie toma `gpu_ranges_mutex` y después uno de los otros, ni lo toma dos
  veces en el mismo hilo, ni toca memoria del guest con él tomado.

**Arreglo (`page_manager.cpp`).** `OpenWindow` con la página ya abierta la vuelve a poner
`ReadWrite` (bajo el lock de la página, después de `windows_mutex`, como `CloseWindows`) y lo
cuenta; `ArmStep` igual. La semántica de la window no cambia: al cerrar, `Protect(Perms())`
restaura la protección que piden los watchers.

**Diagnóstico por defecto (no sólo en la build de prueba).**
- `wait_watch.h/.cpp` (nuevo): `windows_mutex` y `gpu_ranges_mutex` pasan a
  `WaitWatch::Mutex`/`SharedMutex` (mismos locks; anotan dueño y sitio, y sólo registran la
  espera si el `try_lock` falla). También se registran las esperas de un fault por un readback
  del hilo GPU (`ReadMemory` → `SendCommand<true>`/`SendInOrderCommand`) y las del readback por
  la GPU (`scheduler.Wait`/`Finish` en `DownloadMemory`). Un hilo vigía revisa cada segundo; una
  espera de más de 5 s imprime
  `Wait over 5 s (UFC-48; ... module base ...); waits in progress:` y una línea por espera en
  curso: `thread N waits for <qué> for X s (at +off); held by thread M (taken at +off)`, y al
  terminar `Wait over 5 s ended: ...`. Los `+off` se simbolizan con `llvm-symbolizer` sobre el
  `sacredfire-game.exe` con debug info de este commit.
- `NoteRepeatedWriteFault`: un hilo que hace fault 2^20 veces seguidas en la misma página con
  el mismo `rip` imprime una vez `Write fault loop: thread N faulted ... on page ... in X s;
  page open in a write window: yes/no/unknown`. Un loop del juego que hace step sobre un
  contador legítimamente son cientos por segundo (horas para llegar); un reintento infinito son
  cientos de miles por segundo.
- La línea de stats `Write windows:` agrega `N faults on an open window` (cada uno habría sido
  un reintento sobre una página protegida).

### readback-overwrite con window_changed > 0

Los 7 pasan por `DownloadMemory` → `WriteReadback` (el audit sólo corre ahí). No es un readback
que esquive el merge: `WriteReadback` sólo protege bytes de windows **abiertas** o cambiados
desde que **empezó** el readback, y eso es correcto. Lo que el audit no tenía en cuenta es que
después del cierre la GPU puede volver a escribir esos bytes: un binding escrito
(`ObtainBuffer(is_written)`) hace `gpu_modified_ranges.Add` sobre el rango entero, y el
siguiente readback trae el valor de la GPU, **más nuevo** que el de la CPU. El audit seguía
marcando esos bytes como «cambiados por la window» indefinidamente (`age` 509–953 windows
después, segundos): cualquier readback posterior contaba como pisada.

Los casos del log tienen esa forma: páginas enteras de texturas del editor que el juego llena
con `memcpy` (`window_changed` 669–4096) y que después la GPU escribe como imágenes storage
(`[T6a] BC view ... (GPU-written, storage): through memory` en el mismo rango de heap, que se
recicla con cada cambio de pelo); y la de `0x10013fde80+96` (allocator) es un binding de 96
bytes que la GPU escribe (`0x24`) sobre un nodo que el allocator había liberado. Lo que el log
**no** permite es separar cada caso con certeza: el audit viejo no registraba los bindings.

Orden con el hilo GPU retrasado (el submit cierra la window mientras el hilo GPU todavía procesa
una submission anterior): si ese binding hace `Add` después del `Subtract` del cierre, el
`UpdateBuffer` de los bytes de la CPU se graba en el siguiente `Maintain`, después del dispatch:
la VRAM termina con el valor de la CPU y el readback no pisa nada. Si el dispatch es posterior
(otra submission), la GPU escribió después: su valor gana, como en la PS4.

**Arreglos.**
- **Audit preciso** (`page_manager.cpp`, `buffer_cache.cpp`): cada `gpu_modified_ranges.Add` de
  un binding escrito llama `PageManager::DiagnoseGpuWrite`, que pasa los bytes cambiados de
  windows cerradas en ese rango a `reclaimed`. `readback-overwrite` cuenta ahora sólo bytes que
  la CPU cambió en una window cerrada **y que ninguna escritura GPU reclamó después**: esos sí
  son una pisada (el criterio sigue siendo 0). Los reclamados salen como
  `readback-after-gpu-write ... gpu_claimed=N written=M` (informativo, con su propio tope), para
  ver cuántos y dónde. Sólo con `BB_WRITE_WINDOW_DIAG=1`.
- **El camino que sí esquivaba el merge**: las descargas de imágenes del texture cache
  (`DownloadImageMemory`, sync y diferida) escribían directo con `TryWriteBacking`. Con
  `BB_WRITE_WINDOW=1` ahora toman `ReadbackSnapshot` al pedir la descarga y escriben por
  `WriteReadback` (+ audit), como los buffers. Con windows apagadas no cambia nada.

### Compilación

- Linux: `ufc3-build` → exit 0 (PGO off, LTO ON). `write-window-merge-test` y
  `page-watch-perms-test` pasan (compilados bajo el lock de `ufc3-build`).
- Windows: ver más abajo.

### Una prueba

`UFC3-WRITE-WINDOWS-TEST.exe`: Career → editor de luchador → cambiar pelo varias veces, salir
de la categoría cabeza y volver a entrar (ida y vuelta varias veces) → primera pelea de Career
completa; después Quick Fight con peleadores de fábrica, pelea completa. Mandar el log entero,
también si se congela (cerrar el juego **después** de esperar ~10 s congelado, para que salgan
las líneas `Wait over 5 s` / `Write fault loop`).

Qué buscar:
- `Write window audit: readback-overwrite` → **0**. Si aparece alguno, es una pisada real (sin
  escritura GPU posterior): mandar el log.
- `readback-after-gpu-write` → informativo; cuántos salen y en qué páginas.
- `Write windows: ...; N faults on an open window` → N > 0 son casos que antes habrían sido un
  reintento infinito.
- Si se congela igual: las líneas `Wait over 5 s` (qué hilo espera qué y quién lo tiene) o
  `Write fault loop` dicen cuál de las dos cosas es; sin ninguna de las dos, el bloqueo está
  fuera de los locks vigilados (mandar el log igual).

## Quinta pasada: GPU hang en el editor (cs 37aa67f4) y readback-after-gpu-write

Prueba 4 con `UFC3-WRITE-WINDOWS-TEST.exe` de 6a06366
(`~/ufcCOntext/external-logs/UFC48-test4-win-20261016_180706.log`, 923 líneas). Mejoras
confirmadas: 0 `readback-overwrite`, 0 faults sobre una window abierta, sin host fault ni assert.
Final nuevo: en el editor (draws del shader dither/pelo, líneas 869–894) `GPU wait stuck`, el
dispatch `#1648052 cs 0000000037aa67f4, 82x1x1 groups` no termina (895–920), el watchdog de
UFC-48 sólo muestra hilos esperando ese trabajo GPU (12,3 s) y `Device lost during submit` → exit 23.

### Qué es cs 37aa67f4

Está en los dumps de shaders de Linux (`user/shader/dumps/cs_0x0000000037aa67f4*`, también en
`logs/try1b-pro-options`, `ufc45-4k`, `ufc46-1440p`): `spirv-cross` del `.spv` muestra un shader
de morph targets por vértice (64 hilos por grupo; 82 grupos = 5248 vértices). Cuatro bucles
`for (i = 0; int(i) < int(count); ++i)` cuyos `count` (y `start`) salen de tablas `(start, count)`
de 8 bytes por vértice (bindings 1, 4, 6 y 8, 41 816 B cada una = 5227 vértices), sólo lectura;
escribe un buffer de 4 MiB (binding 10). Un `count` con basura (p. ej. `0x3f800000`, un float
1.0 leído como entero: mil millones de vueltas por hilo) es un bucle que no termina en tiempo:
TDR → device lost. Los índices fuera de rango no cuelgan (lecturas robustas devuelven 0): sólo
el `count` puede. En las capturas de Linux (pelea, `BB_WRITE_STEP` apagado: sin steps ni
windows) ese mismo dispatch corre sin colgarse; la tabla del binding 1 es toda ceros allí.

### Causa (por código; el log no tiene los bytes del dispatch)

El hueco es el «límite conocido» de la primera pasada, ahora con un efecto peor: una window no
puede ver una escritura de la CPU que deja **el mismo valor** que el guest viejo sobre bytes en
`gpu_modified_ranges` (R). Ese grano no cambia en el diff, sigue en R, y la GPU lee el valor de
VRAM (no hay upload de bytes R). Para que pase hacen falta las dos cosas que el editor tiene y
la pelea no:

1. la CPU escribe dentro de una window sobre memoria que la GPU escribió (reuso de heap: el
   editor recicla memoria de texturas y salidas GPU con cada cambio de pelo/cabeza; la window
   abre por un primer write a más de 16 bytes de R y después el `memcpy`/bucle sigue hasta R);
2. lo que escribe coincide con el guest viejo (ceros sobre ceros: una tabla `(0, 0)` de vértices
   sin morphs sobre memoria que estaba en cero antes de que la GPU la escribiera).

Medido en los logs: en la Quick Fight de la prueba 1 (~410 windows/s) **todas** las líneas dicen
`CPU took over 0 GPU bytes`; en el editor (pruebas 2–4) la CPU pisa bytes GPU dentro de windows
todo el tiempo (14–98 KB cada 5 s). Ningún diff sirve: decidir de quién es un grano igual exige
saber si la CPU escribió, y eso sólo lo da un fault (readback antes de escribir). Tampoco sirve
un «veneno» en los bytes R al abrir: la CPU puede leerlos durante la window (el allocator lee sus
nodos en páginas con bindings que la GPU marcó escritos sin escribirlos) y leería el veneno.

El modelo está en `write-window-merge-test` (`EqualRewriteUnseen`): pares que cambian llegan a
VRAM, los `(0, 0)` quedan GPU-owned y la GPU lee `0x3f800000` como `count`.

**Qué no queda probado:** que las tablas del dispatch colgado estuvieran en una página así. El
log no tiene sus direcciones en el editor (las de Linux son de pelea), ni hay bytes de VRAM. Lo
prueba el diagnóstico nuevo (abajo) en la próxima corrida.

### readback-after-gpu-write (los ~20 de 677–854)

Se cuentan cuando un binding **escrito** cubre después bytes que la window cambió, y el readback
trae un valor distinto del que dejó la CPU (`written` ≈ `gpu_claimed`: 4028/4058, 3884/3894…).
Si el binding reclamara bytes que el shader no escribe, la VRAM tendría el valor de la CPU
(`ProcessLateWrites` lo sube en el `Maintain` del siguiente paquete, antes de ese dispatch) y el
readback traería ese mismo valor: no contaría como `written`. Que casi todos cuenten significa
que la VRAM tenía otro valor: o la GPU los escribió (válido), o el upload de la window no llegó a
VRAM (bloque no residente o in-place). Para separar las dos cosas el log de stats ahora dice
`N window bytes not in VRAM`: con 0 ahí, los readback-after-gpu-write son escrituras GPU reales.
Las páginas son de texturas del editor que el juego llena con `memcpy` y la GPU reescribe como
storage (`[T6a] ... (GPU-written, storage): through memory`), y `0x10013fd000` es la página del
allocator con contadores GPU de 4 bytes (`cs ba6b230b` liga `0x10013fde80+4` como escrito).
Esto no es el hang: el hang necesita bytes que **nadie** reclamó (siguen R sin cambio).

### Arreglo (de4e15e)

**Sitios vetados** (`write_window_sites.h`, `page_manager.cpp`, `buffer_cache.cpp`
`DecideWriteStep`). Al cerrar una window, si sus cambios tomaron bytes GPU (`NoteSteppedWrite`
ahora devuelve cuántos), su sitio (instrucción que hizo fault + llamador, los mismos de «Write
fault sites») queda vetado: sus faults siguientes sobre páginas GPU van por readback (o step si
es código del juego con pocos bytes GPU y no cerca: el step es exacto), como con windows
apagadas. Sitios que sólo escriben al lado de datos GPU conservan sus windows: en pelea son todos
(0 bytes tomados). Una línea por sitio vetado: `Write windows: site X (from +Y) changed N
GPU-owned bytes of page P in a window (M left GPU-owned beside them); its writes to GPU pages
read back from now on`. Tabla de 256 sitios; llena, no se abren más windows.

Límite: la **primera** window de cada sitio sigue expuesta (es la que delata al sitio), y un
sitio que sólo reescribe valores iguales nunca se delata. Reduce la exposición del editor a una
window por sitio; no la elimina. Si el diagnóstico prueba la causa y esto no alcanza, la opción
exacta es no abrir windows para copias de host (`memcpy`) sobre páginas con más de 256 bytes GPU
(medir antes su costo en pelea) o apagar las windows fuera de pelea.

**Stats** (`Write windows:`): `(Y GPU bytes left GPU-owned beside them)` = bytes R que quedaron
en páginas donde la window tomó otros (la exposición); `Z window bytes not in VRAM`; `W faults/s
of barred sites read back`.

**Diagnóstico (`BB_WRITE_WINDOW_DIAG=1`, lo pone el launcher de prueba).**
- Cada window cerrada guarda los bytes de su página que siguen en R después del cierre
  (`ambiguous`) y cuántos tomó. Un binding escrito posterior los limpia (la GPU los reescribe).
- `PageManager::DiagnoseGpuRead` en cada `ObtainBuffer`: si el binding cubre bytes ambiguos,
  anota el hit con el shader de cómputo que lo liga (`SetDiagShader` en `DispatchRecord` y
  `DispatchIndirectRecord`) en un anillo de 512. Si la window tomó bytes GPU en esa página
  imprime `Write window audit: gpu-reads-ambiguous cs=... binding=... page=... opener=...
  caller=... window_took=N ambiguous=M word=... guest=...` (tope `BB_WRITE_WINDOW_DIAG_MAX`).
- En un hang, debajo del `STUCK` de un dispatch: `write window audit: cs H bound A+S ... over N
  GPU-owned bytes window id=... left unchanged on page P ...` por cada binding reciente de ese
  shader sobre bytes ambiguos, y el total. Si cs 37aa67f4 vuelve a colgar con líneas así sobre
  sus tablas de 41 816 B, la causa queda probada; si no hay ninguna, las windows no tocaron sus
  buffers y la causa es otra.

### Compilación

- Linux: `ufc3-build` → exit 0 (PGO off, LTO ON, `ufc3-v0.1.4-32-g5695dcb-dirty` antes del
  commit; mismo código). `write-window-merge-test` (con `EqualRewriteUnseen` y `BarredSites`),
  `page-watch-perms-test` y `label-guard-test` pasan.
- Windows: `scripts/build-windows.sh --runtime` (bajo el lock de `ufc3-build`) → exit 0,
  `ufc3-v0.1.4-33-gde4e15e`; `scripts/package-windows.sh` limpio. Launcher de prueba con los
  comandos de ninja de `SacredFire.exe` más `-DUFC3_WRITE_WINDOWS_TEST`, sin debug info, en el zip
  como `UFC3-WRITE-WINDOWS-TEST.exe` con `README-UFC48.txt`.
- Paquete: `~/UFC3Native-windows-ufc48-de4e15e.zip` (+ `.sha256`).

### Una prueba

`UFC3-WRITE-WINDOWS-TEST.exe`: Career → editor de luchador → cambiar pelo y cabeza varias veces
(ida y vuelta a la categoría cabeza) → primera pelea de Career completa; después Quick Fight con
peleadores de fábrica, pelea completa. Mandar el log entero, también si se cuelga.

Qué buscar:
- `Write windows: site ... read back from now on`: qué sitios se vetan y cuándo (si alguno se veta
  en pelea, comparar FPS con la prueba 1).
- Sin `GPU wait stuck`/`Device lost`. Si vuelve: las líneas `write window audit: cs ...` bajo el
  `STUCK` (ver arriba) dicen si fue esto.
- `gpu-reads-ambiguous`: qué shaders leen bytes expuestos y dónde.
- `window bytes not in VRAM` = 0 → los `readback-after-gpu-write` son escrituras GPU reales.
- `readback-overwrite` sigue en 0; FPS de pelea como en la prueba 1.
