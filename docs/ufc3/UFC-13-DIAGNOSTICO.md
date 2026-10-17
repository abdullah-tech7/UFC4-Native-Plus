# UFC-13 / T4: tercera pasada (15 oct 2026)

## Resultado y discrepancia de la evidencia

**Sin fix confirmado; se deja instrumentación para una sola prueba corta.** No se
cambia la política de alias ni el render. El log presente en este worktree no
contiene los dos `nonzero=1` citados en el ticket. Para identificar exactamente
el archivo analizado: `logs/20261015_203329.log`, 953966 bytes, SHA-256
`d7ffbdecb3bb0984b13b7f57c99fc4bc3801f9c8568d852d44dda7e9a526bb64`.

| Líneas del log | Dirección / evento | Resultado |
| --- | --- | --- |
| 4267–4269 | `0x1033548000`, RT 1024×2048 antes del rechazo | 1740117 bytes no cero; RGBA 434693,434781,434912,435731 |
| 4271 | misma dirección, upload 864×1360 | 367730 bytes no cero; RGBA visible **0,0,0,0** |
| 4366 | `0x1032f40000`, RT 1024×2048 antes del rechazo | 1009540 bytes no cero; RGBA 252184,252071,252257,253028 |
| 4370 | misma dirección, upload 652×1080 | 191371 bytes no cero; RGBA visible **0,0,0,0** |

Los bytes no cero del upload están fuera del rectángulo visible, en el padding
incluido por `pitch=1024`. No demuestran que la textura recargada tenga contenido
visible. Los conteos del RT completo tampoco demuestran todavía que sus píxeles
estén dentro del rectángulo solicitado: se agrega una medición de esa región.
No se puede concluir con este archivo que el RT ya estaba vacío. Tampoco se
identifica visualmente al peleador sólo por esos conteos.

Se repitió el informe offline con `tools/ufc13_spg_report.py` sobre los captures
actuales, sin modificarlos. En `cap_10001`, `left_large.spg`, `left_medium.spg`,
`right_large.spg`, `right_medium.spg`, `win.spg`, `win_wfa.spg`, los `*_tott.spg` y
los belt siguen con RGB negro y alfa cero. El contenido visible sí se pierde antes
del archivo final; no es sólo una interpretación del JPEG sin transparencia.

## Draws y causa todavía pendiente

Antes del primer rechazo, el log sólo registra los bindings del target
900×1416 `0x1033ff8000` (tick 34391), luego su lectura y el binding del target
1024×2048 `0x1033548000` (tick 34401). No enumera los draws ni sus estados, así
que no permite atribuir VS/PS, depth/stencil, viewport, blend o clears a ese RT.
La línea 3726 muestra PS `39d6a7df` / VS `d9a103aa` con shader dither, pero sobre
CB0 `0x1074dd0000`, 4 muestras; no alcanza para atribuirlo al retrato.

`ResolveOverlap` rechaza la nueva descripción por `BlockDim()`: ésta incluye
pitch y altura, por lo que 1024×2048 y 1024×1360 difieren aunque sean RGBA8 y
compartan tile 14. Ese retorno permite una recarga independiente. El path de
alias BC ya usa `TileImageToBuffer`, pero aplicarlo indiscriminadamente aquí
puede confundir reutilización del pool con un alias vigente. No se introduce
ese cambio sin resolver la discrepancia y medir la región visible.

## Trazas agregadas

Todas requieren `BB_T4_TRACE=1` y se ejecutan en el hilo que registra el render:

- `draw`: hasta 4096 draws en orden, directos e indirectos, hacia targets de
  ancho 600–1024 y alto 1000–2048. Incluye todos sus MRT: dirección, formato,
  tile, mip, muestras guest/backing de pipeline, clear y valor, write mask
  efectivo y factores/operaciones de blend. También VS/PS y variante PS,
  tamaño del framebuffer, viewport/scissor efectivos Vulkan, cull/discard,
  depth asociado y estado efectivo de depth/stencil, bounds y clears.
  `count`/`instances` son registros guest; en draws indirectos los argumentos
  efectivos vienen del buffer indirecto, no de estos campos.
- `db_alpha_to_mask`/`a2m_disable` y `a2c` registran estado guest y coverage
  del pipeline; **el registro por sí solo no prueba shader dither**. Correlacionar
  PS/variante/target con los mensajes `[T7]` existentes.
- `fast-clear`: hasta 128 clears realmente ejecutados por eliminación de fast
  clear, con dirección, CMASK, modo y valor. Los clears de attachments se ven
  en `draw`.
- `cb-resolve`: hasta 128 resolves explícitos, con origen/destino y muestras.
- `image-copy`, `image-resolve`, `backing-samples`: hasta 256 eventos compartidos
  del runtime para esas dimensiones; incluyen muestras de backing y si la
  conversión copia contenido. Cubren conversiones implícitas además de CB_RESOLVE.

Además, con **ambos** `BB_T4_TRACE=1 BB_T4_TRACE_PIXELS=1`, `alias-region-host`
cuenta RGBA dentro de `[0,width) × [0,height)` de la textura solicitada, usando
la misma copia staging del RT. No agrega copias ni esperas; se mantienen los
límites de las probes anteriores (16, dos por evento/dimensión, 8 MiB cada una).
Con flags apagados no hay nuevas transferencias ni esperas. Las probes con
píxeles siguen haciendo `scheduler.Finish()` y pueden alterar tiempos.

## UNA prueba siguiente

Iniciar el comando habitual con
`BB_T4_TRACE=1 BB_T4_TRACE_PIXELS=1 BB_SAVE_LOG=1`, crear **un solo peleador nuevo**
y guardar hasta generar un `cap_*` nuevo; cerrar normalmente. Entregar ese log
y ese capture. No hace falta una pelea.

Seguir por dirección/tick los `draw` y transferencias hasta el rechazo. Si
`alias-region-host` tiene RGB/alfa y `alias-upload-host` vuelve a cero, la pérdida
está en la transición de descripción. Si la región ya es cero, los estados de
los draws, clears y resolves permitirán seguir el productor del RT. Si el
runtime llega al límite de trazas, la ausencia de líneas posteriores no prueba
que no hubo operaciones. Queda pendiente correlacionar por separado el modelo
del jugador en post-fight.

## Validación de esta pasada

La compilación final con `ufc3-build` terminó con código 0 y produjo
`out/sacredfire`, versión `ufc3-v0.1.2-57-ga3cae11-dirty`. Se recompilaron
`vk_rasterizer.cpp`, `vk_runtime.cpp` y el caché de texturas. Sólo se observaron
los avisos de CMake/ODR de fsr-vulkan ya presentes; no hubo errores.
`git diff --check` pasó. No se ejecutó el juego ni se hicieron commits, cambios
de rama, merge, rebase o push. No se tocó online, `vk_pipeline_cache.*` ni
`shader_recompiler/**`. Los symlinks `orig`/`user` no forman parte del diff.
El submódulo fsr-vulkan ya figuraba modificado al inicio y se mantiene fuera
del cambio del ticket.

---

## Segunda pasada (histórico)

# UFC-13 / T4: segunda pasada (15 oct 2026)

## Resultado

**No hay un fix seguro confirmado.** La descarga final sí existe; el fallo está
presente en los píxeles host antes de escribir el archivo. Se acota una hipótesis
concreta de pérdida del render target al cambiar su descripción a textura con
mips. No se cambia la política de coherencia ni se habilita memcpy de imágenes
tiled a memoria guest. Se conserva el merge `8dcae5d`.

Fuente: `logs/20261015_201651.log` del worktree, build reportado
`ufc3-v0.1.2-55-g8dcae5d`, más sus CSV y los captures disponibles. Las referencias
siguientes son números de línea de ese log (12082 líneas al inspeccionarlo).

## Captures decodificados y comparación

Se decodificaron los **100 SPG** de `cap_10000`, `cap_20001` … `cap_20004`, sin
modificar los originales. Se recorre el PNG hasta IEND tras la cabecera de 12
bytes y se abre después el JPEG; se comparan los píxeles de ambas imágenes.
El informe completo con dimensiones, extremos, número de píxeles alfa no cero
y SHA-256 de cada plano está en `UFC-13-SPG-20261015.csv`. Reproducción:

```sh
python tools/ufc13_spg_report.py user/temp0/createplayer > /tmp/ufc13-spg.csv
```

- Todos los medium (652×1080), win/win_wfa (600×1080) y tott/belt (1000×1200)
  tienen **RGB negro y alfa cero**, en los cinco directorios. No es solamente
  un JPEG negro o una transparencia inferida del tamaño comprimido.
- Los large (864×1360) son negros con alfa cero excepto `cap_20002`, que contiene
  bloques corruptos y 116146 píxeles alfa no cero. `cap_20004/left_large.spg`
  vuelve a ser negro/transparente (20219 bytes).
- La inspección visual del color de `cap_20004/front.spg` (45861 bytes) y
  `left_small.spg` (157565 bytes) muestra bloques/ruido, no un retrato reconocible.
  Tener alfa o bytes no cero no equivale a haber renderizado un peleador válido.
- Frente al diagnóstico anterior, medium/win/tott mantienen las dimensiones,
  tamaños y contenido nulo descritos allí; los large de `cap_20002` mantienen
  el tamaño 286429 y corrupción. No queda una copia independiente de los
  archivos anteriores para certificar identidad binaria entre sesiones.
- Las escrituras tempranas de `cap_10000` … `cap_20003` (L1333–1743) son anteriores
  al lote de render/exportación observado. No deben atribuirse a ese lote.
  `cap_20004` se crea en L5801 y sus SPG se escriben en L5807–5978.

## Cadena de imágenes observada

Todas las superficies de esta tabla son RGBA8 UNORM (`R8G8B8A8Unorm`), una capa,
una muestra. Las fuentes tiled son RT y/o storage; los destinos lineales son
storage. Los enlaces entre bindings son candidatos por orden y dimensiones:
la traza previa no enumera recursos por dispatch ni enlaza el buffer de entrada
del encoder con el destino GPU. No se afirma una correspondencia de direcciones
con cada nombre left/right/belt individual.

| Dimensión / evento | Dirección guest | Pitch / tile | Evidencia |
| --- | --- | --- | --- |
| RT inicial large 900×1416 | `0x1030368000` | 1024 / 14 tiled | RT tick 31539; sampled 31547 |
| RT padded 1024×2048 | `0x102f8b8000` | 1024 / 14 tiled | RT L5262 tick 31547; una mip, flags 0x48 |
| Textura large 864×1360 sobre esa dirección | `0x102f8b8000` | 1024 / 14 tiled | L5263: 11 mips, flags 0x40; luego storage 0x48 |
| Export large 864×1360 | `0x1083d8c800` | 864 / 8 lineal | L5290 storage tick 31549; L5291 download-host tick 31550: **0/4700160 bytes no cero**; L5292 guest-written |
| RT medium 652×1080 | `0x107a2b0000` | 768 / 14 tiled | L5308 tick 31603; sampled L5309 |
| RT padded → textura medium | `0x102f2b0000` | 1024 / 14 tiled | L5310 RT 1024×2048; L5311 textura 652×1080, 2 mips y flags 0x40; crece a 11 mips |
| Export medium 652×1080 | `0x1083ad8a00` | 656 / 8 lineal | L5333 tick 31606: 13530/2833920 bytes no cero; L5334 guest-written |
| RT inicial win 500×900 | `0x103e7e8000` | 512 / 14 tiled | L5711 RT tick 32729; sampled L5712–5713 |
| RT padded → textura win | `0x1034890000` | 1024 / 14 tiled | L5714 RT 1024×2048; L5715 textura 600×1080, 2 mips y flags 0x40; crece a 11 mips |
| Export win 600×1080 | `0x1083f86c00` | 608 / 8 lineal | L5736 storage tick 32736; L5737 tick 32737: **0/2626560 bytes no cero**; L5738 guest-written |
| Segundo export 600×1080 | `0x1083f86c00` | 608 / 8 lineal | L5793 tick 33121: 48 bytes no cero; L5794 guest-written; fuente candidata `0x1034558000` |

`UFC-13-READBACKS-20261015.csv` detalla los **20 exports** entre L5291 y L5793,
incluyendo pequeños, front, tott y demás medium. Todos tienen `guest-written`
inmediatamente después del `download-host`; no hay un `guest-write-failed`
en ese lote. Los tiled tienen `linear_eligible=0`, pero el juego ya produce
explícitamente un destino lineal que **sí** se descarga. No hace falta descargar
todo RT tiled para que funcione esta ruta.

La descarga y escritura guest de large (L5291–5292) preceden al primer write SPG
nuevo (L5807); la de win (L5737–5738) precede al write de `win.spg` (L5951).
Esto no fecha la primera lectura del CPU que alimenta el encoder: esa lectura
no está instrumentada. No permite excluir toda carrera CPU, pero una descarga
más temprana del mismo destino tampoco puede recuperar contenido que ya está
en cero en staging. El hash original incluye padding; los 13530 bytes del medium
no demuestran alfa ni color visibles válidos.

## Causa acotada y límite de la evidencia

La transición RT padded → textura con mips es el primer punto sospechoso concreto.
En `TextureCache::ResolveOverlap`, para la misma dirección, la comparación
`BlockDim()` incluye pitch y altura; 1024×2048 y 600×1080/652×1080/864×1360 no
coinciden. Se retorna sin expandir ni copiar el contenido GPU. `FindImage` puede
crear otra imagen y `RefreshImage` cargarla desde memoria/buffer cache. La traza
muestra el RT GPU-modified (flags 0x48) seguido de una imagen sampled solamente
Registered (0x40) en la misma dirección, exactamente antes de generar mips.

**Hipótesis principal:** el RT contiene el retrato, pero el alias carga datos
viejos/vacíos al cambiar dimensiones. Todavía no se sabe si el RT anterior
contenía un peleador visible: un binding RT no prueba draws ni canales válidos.
Tampoco se conoce el swizzle/vista efectiva en la traza anterior. No se confirma
un bug de shader, alfa, vista ni un modelo que nunca se dibuja.

Un `CopyImage` rectangular o ampliar automáticamente el RT con nuevas dimensiones
no es seguro sin comprobar el layout: los mips/pow2/pitch describen memoria tiled,
y los formatos/tile modes iguales no garantizan que coincida toda esa memoria.
La infraestructura `TileImageToBuffer` permite preservar el layout guest dentro
del GPU, pero habría que acotar cuándo esas descripciones son alias reales y
cuándo son reutilización de un pool. No se generaliza esa política con este log.

## Qué se puede afirmar del post-fight

El juego consume los captures vacíos: por ejemplo `left_wfa.spg` en L6000 y
`right_wfa.spg` en L8163, L9120, L9745 y L11657. `win.spg` se lee en L6238,
L10276, L10846 y L11421, pero en esos lugares se recorre el directorio entero;
esas lecturas pueden ser empaquetado del save y no uso visual del menú.
Hay escritura/lectura de `NISPOSTFIGHTCELEB_94.jpg`, pero no un marcador inequívoco
de POST FIGHT MENU. **No se puede asignar una imagen/draw al modelo del jugador
del post-fight ni asegurar que use win.spg.** La ausencia del jugador comunicada
por el usuario sigue pendiente de correlacionar con draws concretos.

Los `.readbacks.csv` registran coherencia de buffers/faults, no los planos
completos de estas imágenes; `.frames.csv` agrega draws/dispatches. No identifican
qué draw corresponde a cada peleador. Se usan las trazas de imagen para afirmar
la descarga; los CSV no cubren esa atribución por personaje.

## Instrumentación nueva y UNA prueba siguiente

Se agregan trazas detrás de `BB_T4_TRACE=1`:

- `rgba_nonzero=R,G,B,A`: píxeles visibles por canal para RGBA8, excluyendo padding;
  el conteo/hash original de todos los bytes sigue presente.
- `alias-rejected-blockdim`: descripción vieja/nueva, dirección, binding y tick
  de la última escritura GPU del RT padded (máximo 64 mensajes).
- `view`: dimensiones solicitadas/resueltas, formato, rango mip/capa y swizzle
  de las texturas de retrato (máximo 256 mensajes).

Además, **sólo con `BB_T4_TRACE_PIXELS=1` junto a `BB_T4_TRACE=1`**, se copia a
staging host el nivel 0 del RT al rechazar el alias (`alias-before-reject-host`)
y de la textura recién cargada (`alias-upload-host`). Son probes RGBA8 de una
capa/muestra, máximo 8 MiB cada uno, 16 probes en total y dos por evento/dimensión.
No hay writeback guest ni retiling. Estos probes hacen `scheduler.Finish()`;
pueden cambiar tiempos, por lo que sirven para localizar pérdida de contenido,
no para descartar una carrera por observar que desaparece con instrumentación.
Con ambos flags apagados no se hacen estas copias ni esperas.

**Una prueba:** iniciar el comando habitual con
`BB_T4_TRACE=1 BB_T4_TRACE_PIXELS=1 BB_SAVE_LOG=1`, crear **un solo peleador nuevo**
y guardar hasta que se genere un `cap_*` nuevo; cerrar normalmente. No hace falta
jugar otra pelea para esta prueba. Entregar el log y ese capture nuevo. Comparar,
para el mismo domicilio del RT/alias, `alias-before-reject-host` con
`alias-upload-host` y el export lineal posterior:

- RT con canales no cero → alias vacío aporta evidencia de pérdida en la
  transición del caché; antes de fijarla hay que comprobar que esos píxeles
  pertenecen a la región visible del retrato y no sólo al padding del RT.
- RT ya vacío sitúa el fallo antes de esa transición.
- Alfa/color presentes en ambos, pero ausentes en export, sitúan el fallo en la
  conversión posterior; los `view` indican qué rango/swizzle se solicitó.

Estos conteos no sustituyen una captura visual para demostrar que el contenido
es un peleador correcto, pero distinguen el punto donde se vuelve cero.

## Validación de la segunda pasada

`ufc3-build` completó con código 0, incluida una segunda compilación de la
versión final de las trazas. Generó `out/sacredfire`, versión
`ufc3-v0.1.2-55-g8dcae5d-dirty`. El análisis offline decodificó 100/100 SPG y
`git diff --check` no encontró errores. El build emitió los avisos existentes
de CMake y ODR de fsr-vulkan; no falló.
No se ejecutó el juego, no se tocó online, `vk_pipeline_cache.*` ni
`shader_recompiler/**`, y no se usó `git add` ni `git commit`.

---

## Primera pasada (archivo histórico)

# UFC-13 / T4: captures de career y jugador ausente

## Resultado de la investigación (15 oct 2026)

No hay un fix confirmado. Se deja instrumentación opt-in `BB_T4_TRACE=1` para una
prueba manual. No se ejecutó el juego ni se cambiaron las políticas de readback.

Evidencia de `logs/20261015_195349.log` del checkout principal:

- Línea 2: `linear image readbacks 1`. `run.sh` ya activa `BB_READBACK_LINEAR=1`
  para UFC 3; el default genérico del shim sigue siendo false.
- `FindTexture` ya encola **todas** las storage lineales con dirección guest,
  incluyendo 2D >=64x64. El filtro de mancelli `e565b664` restringiría esa
  cobertura; no agrega un caso que falte aquí. También encolamos RT lineales.
- Las descargas se procesan antes de publicar fences EOP/EOS y en `OnSubmit`.
  `DownloadImageMemory` copia píxeles lineales: habilitar imágenes tiled allí
  sin retiling escribiría un layout incorrecto en memoria guest.
- Líneas 1111 y 1567: falla `open(/temp0/images/career)`; línea 1595:
  falla `open(/temp0/images/match)`. Ambas carpetas están ausentes en los datos
  inspeccionados. `runtime_file_configure` ya intenta crearlas desde `327737c`.

Los archivos `user/temp0/createplayer/cap_*/*.spg` tienen cabecera de 12 bytes,
seguida de un PNG grayscale de alfa y un JPEG de color. Se decodificaron ambos
con Pillow (no basta comparar tamaños de archivos):

| Capture | Dimensiones | Bytes | Contenido en cap_20002 |
| --- | --- | --- | --- |
| `win.spg`, `win_wfa.spg` | 600x1080 | 11683 | RGB negro y alfa cero en todos los píxeles |
| `left_medium.spg`, `right_medium.spg`, belt/wfa medium | 652x1080 | 12554 | RGB negro y alfa cero en todos los píxeles |
| `*_tott.spg`, `*_tott_belt.spg` | 1000x1200 | 20782 | Alfa cero; imagen invisible |
| `left_large.spg`, `right_large.spg` | 864x1360 | 286429 | Bloques/bandas de datos corruptos, no un retrato válido |
| `front.spg` | 360x260 | 136060 | Bloques de datos corruptos, no un retrato válido |

Los medium, win y tott también tienen alfa cero en `cap_10000` y `cap_20001`.
En esos dos directorios los large también son transparentes (20219 bytes).
Los captures se escribieron a las 19:54:37 (`cap_10000`, `cap_20001`) y
19:55:37–38 (`cap_20002`), antes del final de la sesión.

Esto confirma que el pipeline que produce/guarda retratos ya entrega imágenes
vacías o corruptas. No demuestra que el post-fight use `win.spg`, que falte un
draw del modelo, ni que tiled readback sea la causa. Los CSV no identifican el
archivo ni el formato/tiling de la imagen de origen: `.readbacks.csv` registra
faults/copias de buffers, no las descargas de `DownloadImageMemory` con su layout;
`.frames.csv` da draws/dispatches agregados, no por peleador. No hay un marcador
inequívoco de entrada al post-fight. Tampoco se puede atribuir la ausencia de las
carpetas a un fallo de creación: podrían haber sido eliminadas por el guest.

## Instrumentación

Con `BB_T4_TRACE=1`, todas las líneas nuevas llevan `[T4]`:

- `texture_cache.cpp`: bindings `render-target`, `storage`, `sampled` con tick,
  dirección, dimensiones, pitch, formato Vulkan, tile mode, flags, mips, capas y
  muestras. `linear_eligible` indica si cumple la política actual de descarga
  automática; un binding `sampled` por sí mismo no encola una descarga.
- `download-host`: staging completado, bytes no cero y hash XXH3 antes de la
  escritura guest; `guest-written` / `guest-write-failed`: resultado real de
  `TryWriteBacking`. `download-in-place` identifica la ruta sin copia CPU.
  No se agregan esperas GPU ni lecturas de memoria guest protegida.
- GPU: sólo imágenes 2D de color no BC >=64x64, hasta 512 combinaciones de
  evento/layout, 32 mensajes por combinación. El conteo de bytes incluye padding;
  un hash no cero no demuestra que el retrato sea válido. La ruta asíncrona de GC
  no tiene análisis de staging; las descargas encoladas usan la ruta síncrona.
- `runtime_file.c`: resultado de crear carpetas al configurar, opens con flags y
  errno negativo, primer read/write por descriptor con dirección del buffer,
  tamaño al cerrar, mkdir/rmdir/unlink/rename de captures y `temp0/images`.
  Esto permite saber qué `.spg` se genera y cuál se vuelve a abrir en cada menú.

## Una prueba manual pendiente

En una sola sesión, iniciar el build con `BB_T4_TRACE=1 BB_SAVE_LOG=1` (por ejemplo
antepuestos al comando habitual de `run.sh`, conservando el directorio del juego
y los datos habituales). Crear **un nuevo peleador de career**, guardar para
generar captures nuevos, abrir pause, ganar una pelea y quedarse unos segundos
en POST FIGHT MENU. Cerrar normalmente y entregar el `.log`, sus dos CSV y el
directorio `temp0/createplayer/cap_*` recién generado. No borrar saves ni reutilizar
solamente captures anteriores: una imagen ya guardada en blanco no se repara
con un cambio posterior de readback.

Buscar las dimensiones anteriores (o sus superficies intermedias/padded): si se
escriben en storage tiled sin conversión a lineal/descarga, investigar retiling
y coherencia CPU sobre esa dirección, en lugar de ampliar el memcpy lineal.
Si se completa `download-host` y `guest-written` pero el `.spg` sale vacío,
seguir el origen del render, sus canales y la conversión/codificación. Los opens
en post-fight permitirán comprobar si consume ese capture. Si el jugador falta
sin leer captures, hará falta inspeccionar los draws del modelo: este diagnóstico
no pretende resolver esa posibilidad con un cambio especulativo al texture cache.

## Validación del cambio

`ufc3-build` completó con código 0 para la versión final de la instrumentación;
se generó `out/sacredfire` (`ufc3-v0.1.2-50-g9ebeb1e-dirty`). `git diff --check`
sin errores. No se ejecutó el juego. La compilación aplica los parches existentes
del repo al submódulo fsr-vulkan; no se agregaron cambios propios allí. No se tocó
online, `vk_pipeline_cache.*` ni `shader_recompiler/**`. No se hizo commit.
