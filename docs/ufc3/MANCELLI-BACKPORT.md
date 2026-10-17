# Backport del fork shadPS4 de mancelli → `gpu/shadps4` (UFC-3, 15 oct 2026)

Fork analizado: `~/ufcCOntext/mancelli-shadPS4`, tag `ufc4-intel-arc-2026-10-14` (`12739d23`).
El fork **no** sale de shadPS4 directo, sino de **Shadlix** (diegolix29, ~1500 commits propios
de launcher/emulador); el merge-base con upstream es `3912336f` (1 oct). Lo propio del fork son
61 commits (`git log --no-merges TAG ^upstream/main ^origin/Shadlix`): 51 de mancelli (todos
co-escritos con Claude) y 10 de "Claude"; 11 de ellos son cherry-picks de PRs upstream
recommiteados por mancelli.

Nuestro árbol: `gpu/shadps4` = upstream `b4e7ae77` (24 sep) + 9 backports del recompilador
(ver `gpu/VENDOR.txt`) + todo lo de bbport/SacredFire/ufc3 (`bbport:`/`ufc3:`). El recompilador
está casi intacto respecto de `b4e7ae77` (diffs de 0–35 líneas por archivo); `video_core` está
muy divergido (vk_rasterizer.cpp: 3455 líneas cambiadas, page_manager.cpp: 680, write windows,
write step, preupload, guest in place, async pipelines…). Archivos de Shadlix que el fork toca y
nosotros **no tenemos**: `storage_image_sync.cpp`, `region_manager.h` con "Readback Speed",
`config.cpp`/Qt.

Método: cada parche (rutas `src/` → `gpu/shadps4/`) se probó con `git apply --check` sobre
nuestro árbol, y la serie upstream se aplicó en orden sobre una copia en `/tmp/ufc3-mancelli/tree`
(no se tocó el repo). Para los PR upstream conviene portar **el commit original upstream**, no
la re-aplicación de mancelli (que trae contexto de Shadlix): casi todos aplican limpio así.

## Lo que dice el reporte vs. lo que hay

- **"Career mode fully playable"**: en el fork no hay ni un commit de save data; el career de
  UFC 3 funciona ahí por las librerías HLE de Shadlix. En nuestro port el save data es nuestro
  runtime (`src/runtime_savedata.c`), no shadPS4: el error "Your career data will not be saved"
  lo arreglan los parches AMD 0014–0016 (`sceSaveDataLoadIcon`, mount RW que crea). Nada que
  portar del fork para eso. "Created fighters keep their avatars" es UFC 4 (`e565b664`).
- **"Hair bug fixed"**: es UFC 4 (DB_ALPHA_TO_MASK, `69ab1996`) más NaN en Intel/NVIDIA. Hay
  que verificar que UFC 3 use alpha-to-mask en el pelo antes de portarlo (ver T7).
- **Sombras**: la sombra lejos de los peleadores / piso sin luz que arregla `72251298` es el
  mismo bug de depth leído como R16_UINT que ya resolvimos en 0.1.2 (`243add7`, DepthAsR16 por
  capa). Si el usuario probó 0.1.1 o antes, lo vio. Lo que queda del fork es el ajuste de depth
  bias (`c08218db`) y la versión general D32→R32_UINT.
- **Creación de personaje**: lo más valioso del fork para nosotros está acá (`01b32f45`,
  `5b0e9954`, `789551b0`): texturas comprimidas en GPU y flush de imágenes GPU antes de escrituras
  CPU. Es el candidato principal para "la piel se rompe al cambiar el tono" y encaja con lo que
  vio el tester AMD (BB_PREUPLOAD / BB_WRITE_STEP empeoran la piel = escrituras CPU sobre memoria
  de una imagen que solo existe en GPU).

## Tabla

Esfuerzo: **S** < ½ día (aplica limpio o casi), **M** 1–2 días (merge a mano), **L** > 2 días
(rediseño contra nuestro texture cache / page_manager). Autor = quien hay que acreditar en el
commit del backport (GPL-2.0-or-later, igual que el vendor; conservar `Co-Authored-By` de Claude
donde el original lo tiene). mancelli = `mancelli <129538435+mancelli@users.noreply.github.com>`.

### Commits propios del fork (GPU)

| Commit | Autor | Qué arregla | Síntoma UFC | ¿Aplica a nuestro árbol? | Esf. | Conflictos (AMD / page_manager / write windows) |
|---|---|---|---|---|---|---|
| `01b32f45` texture_cache: GPU-compressed textures through memory, flush GPU images before CPU writes | mancelli | (1) vista BC sobre imagen no comprimida (compresión BC en GPU vía storage R32G32B32A32_UINT) pasa por memoria (TileImageToBuffer → buffer cache → upload BC) en vez de `ExpandImage`; (2) write back de storage aliases de imágenes BC; (3) fault de escritura CPU sobre una imagen solo-GPU (RT/storage) la vuelca al buffer cache antes de reintentar; (4) vista de formato compatible con más mips expande en vez de tirar. UFC 4 CF: piel gris | **Creación** (piel negra/rota al cambiar tono), probablemente también parches negros en career | **Parcial**: nuestra `ResolveOverlap` tiene el mismo `ExpandImage` viejo (texture_cache.cpp:376) y `TileManager::TileImage` existe, así que (1)(2)(4) se portan a mano. (3) no aplica tal cual: depende de `storage_image_sync.cpp` (Shadlix) y de la firma `InvalidateMemory(addr,size,assume_locks,from_fault)`; la nuestra es `(…, bool* step)` | **L** | **Choca con page_manager/write windows**: el flush cuelga del fault de escritura y asume que la página sigue protegida y el reintento vuelve a fallar. Con write windows abiertas (`BB_WRITE_WINDOW=1`) o write step la CPU escribe sin fault y el flush nunca corre; con `BB_GUEST_IN_PLACE` el "buffer cache" es la memoria del juego. Usa `SendCommand<true>` al hilo GPU desde el handler: riesgo de deadlock con nuestros locks (mancelli lo evita con `!assume_locks`). AMD 0002 (crear BC sin Storage) es compatible: la escritura va por la imagen no comprimida |
| `5b0e9954` texture_cache: lazy write back of storage aliases | mancelli | El write back de (2) en cada dispatch costaba hasta 10 GB/s en CF; ahora marca `PendingWriteBack` y vuelca al subir un alias o liberar la imagen (−87 %) | Creación (rendimiento/stutter editando) | **Parcial**: depende de `01b32f45` y de `storage_image_sync.cpp` | M (tras 01b32f45) | Mismo que arriba; además lectura CPU del rango ve datos GPU recién tras el flush (avisado por el autor) |
| `789551b0` vk_rasterizer: expand fast clear on FMASK decompress | mancelli | FMASK decompress se saltaba; ahora corre `EliminateFastClear`, y además limpia una imagen MSAA que la GPU nunca dibujó (contenido indefinido = piel del peleador anterior / grilla de puntos en capa de tatuajes) | **Creación** (piel/tatuajes con basura) | **Sí** (merge a mano, misma estructura: vk_rasterizer.cpp:496–504 y 693). Upstream ya tiene la mitad: **#5283 `14ada48a`** (1 línea) | S | Ninguno con AMD |
| `e565b664` texture_cache: download linear storage images written by compute | mancelli | Imágenes 2D lineales escritas por compute (foto del peleador convertida a lineal para que la CPU la codifique) nunca se bajaban → avatares negros en el save (UFC 4) | **Career** (retrato del pause menu en blanco, `temp0/createplayer/cap_*` iguales y blancos) | **Parcial / ya casi está**: tenemos el mecanismo (`readback_linear_images`, texture_cache.cpp:783) pero apagado por defecto (`BB_READBACK_LINEAR`, emulator_settings.h:66) y sin filtro. El parche lo activa solo para 2D lineal ≥64×64, no depth, no BC | S | Ninguno; cuesta un wait de GPU por dispatch que escribe una imagen así (raro). **Probar primero `BB_READBACK_LINEAR=1` sin código** |
| `72251298` video_core: bind a color copy when a shader reads depth as a color format | mancelli | Depth leído con T# R16_UINT/R32_UINT: alias color convertido en cada bind; libera el alias con su imagen | Sombras (sombra lejos del peleador en Arc, piso sin luz en 780M) | **Parcial, ya resuelto**: nuestro `DepthAsR16` (vk_rasterizer.cpp:2910–3340, `243add7`, + AMD 0003 vista 2D por capa) hace lo mismo para R16 por capa. Falta: D32→R32_UINT y liberar la copia al morir la imagen (hoy `state.copies` nunca se borra, clave = `VkImage*`) | S | Se superpone con AMD 0003 (quedarse con 0003 + agregar el caso R32) |
| `c08218db` vk_rasterizer: scale depth bias for emulated D16S8 depth | mancelli | Bias constante ×256 cuando D16S8 se emula con 24/32 bits: acné/parpadeo de sombras | Sombras (acné, parpadeo) | **Parcial**: aplica limpio. Nuestra emulación por defecto es **D32_SFLOAT_S8** (vk_instance.cpp:993); ahí la unidad del bias es 2^(e−23) según el exponente de la primitiva, o sea 2^-24 en profundidades 0.5–1: ×256 es aproximadamente correcto ahí y exacto en D24S8. Pero con **`BB_Z16S8_AS_D16=1`** (AMD 0004) `GetSupportedFormat` devuelve D16, el parche lo toma como emulado y multiplica ×256 un D16 real | S | **Conflicto con AMD 0004**: decidir el factor según el formato host real de la imagen (D16 → ×1; D24S8 → ×256; D32F → ×256 aproximado, o `VK_EXT_depth_bias_control` con representación fija si el driver la tiene) |
| `69ab1996` Emulate DB_ALPHA_TO_MASK (alpha to coverage) | mancelli | DB_ALPHA_TO_MASK (reg 0x2DC) se ignoraba: tarjetas de pelo con alpha a cobertura salían planas y con borde duro. MSAA → `alphaToCoverageEnable`; 1 muestra → discard por umbral dither 2×2 en el FS | **Pelo** (UFC 4 confirmado; UFC 3 **sin verificar**) | **Sí, a mano**: falla en runtime_info.h, vk_pipeline_cache.cpp y la serialización (tenemos campos y versiones de SacredFire). El registro ya está declarado (regs_depth.h:173) | M | Ninguno con AMD. Cambia la clave de pipeline y runtime info → bump de `ShaderMetaVersion`/`PipelineKeyVersion`; ojo con async pipelines y `vk_shader_hle` |
| `630b4e70` vk_rasterizer: report CMASK tiles as written for shader reads | mancelli | Compute que mezcla la UI solo en tiles con CMASK escrito: UI invisible a resolución nativa / modo Pro (UFC 4). Llena el CMASK con 0xFF en cada binding de solo lectura | Otro (UI/modo Pro) | **Parcial**: AMD 0011 resuelve lo mismo en UFC 3 (modo Pro funciona). Este cubre bindings no-texel; el AMD solo texel | S | **Se superpone con AMD 0011** (ver comparación) |
| `8034718c` vulkan: Intel subgroup handling | mancelli | Intel: subgroup 32, workgroups 1D, guard de NaN en exports/storage; **todas las GPU**: barreras de subgrupo en bloques LDS dentro de control de flujo divergente; LDS barriers para hosts con subgrupo < 64 | Otro (cuadros negros/piso/sombras en Intel Arc) | **Parcial**: lo Intel no aplica a nuestro público (Deck/AMD/NVIDIA). La parte "divergente" sí es útil sobre #5285. Aplica mal (Shadlix + Intel) | M (solo la parte LDS: S) | Se superpone con AMD 0008 (ver comparación) |
| `731a29e7` enable the NaN guard and 1D workgroups on NVIDIA too | mancelli | Lo mismo para NVIDIA, sin probar | Otro (cuadros negros en NVIDIA) | No por ahora: el usuario NVIDIA (HeuZ) no reporta cuadros negros; depende de 8034718c | M | — |
| `02d3b06d` shader_recompiler: NaN-aware min/max/clamp matching GCN | mancelli | `v_min/v_max/clamp` devuelven el operando no-NaN como GCN (NMin/NMax) | Otro (cuadros negros por NaN en blur/TAA; pelo/piel en Intel) | **Sí**: aplica limpio solo; tras #5257 hay que rehacer 2 hunks (4 funciones FMax/FMin → NMax/NMin) | S | Ninguno |
| `5dedb48f` runtime_info: give num_allocated_vgprs 9 bits | mancelli | Bug de #5257: `num_allocated_vgprs : 8` trunca 256 → 0 y el traductor pisa v0 (antes assert "Out of VGPRs"). **Upstream main sigue con el bug** | Otro (shaders corruptos al azar) | **Sí, obligatorio junto con #5257**; aplica limpio tras él | S | — |
| `84c6ede1` vk_rasterizer: fault storm self-heal | mancelli | 64 faults de escritura seguidos en la misma página → log del estado y página escribible (UFC 4: congelado con sonido) | Otro (congelamientos) | **No tal cual** (page_manager distinto); la idea (diagnóstico + autocura) es útil para nuestros write watchers | M | Toca exactamente page_manager/write windows; sería un ticket propio de diagnóstico |
| `e669792b` persist VkPipelineCache, atomic shader cache writes | mancelli | Guarda el `VkPipelineCache` en disco; escrituras atómicas de caché | Otro (stutter de shaders) | **Parcial**: no tenemos `getPipelineCacheData`; vk_pipeline_cache.cpp muy divergido (async pipelines) | M | — |
| `a29dc76d` check shader patches once per permutation | mancelli | Rendimiento al compilar | Otro | Dudoso (nuestro flujo de patches difiere, vk_pipeline_cache.cpp:1046) | S | — |
| `6a70c913` SHADPS4_GPU device pick | mancelli | Elegir GPU con caché por GPU | Otro | No prioritario | S | — |

### PR upstream que trae el fork (portar el original upstream)

| Commit upstream (fork) | PR · Autor | Qué arregla | Síntoma UFC | ¿Aplica? | Esf. | Conflictos |
|---|---|---|---|---|---|---|
| `c34ca21f` (`caf3d7c2`) | #5220 · artemkaVG | EXEC tras rama vacía | Otro (sombreado erróneo puntual) | **Sí, limpio** | S | — |
| `c4952908` (`5157080d`) | #5257 · TheTurtle | NaN GCN: modo legacy, DX10 clamp, med3 con NaN, clamp con NClamp | Otro (NaN → cuadros negros) | **Sí** con `-C1`; requiere **`OpNClamp` en nuestro sirit** (`gpu/third_party/sirit` solo tiene NMin/NMax; el PR sube el submódulo) + `5dedb48f` | S | Bump de `ShaderBinaryVersion`/`ShaderMetaVersion` (las nuestras van por 8) |
| `1c2b8f97` (`5f7d8a11`) | #5346 · DanielSvoboda | V_RCP_CLAMP/V_RSQ_CLAMP/V_RSQ_LEGACY/V_LOG_CLAMP | Otro (Inf/NaN en iluminación) | **Sí, limpio** | S | — |
| `c5dd8b8b` (`7e0d7b9a`) | #5197 · kiwakiwaa | Factores de blend ZERO/ONE con MIN/MAX | Otro (blending de partículas/UI) | **Sí, limpio** (el de mancelli no) | S | — |
| `10393d2c` (`36c9f6c3`) | #5274 · Chris Chreece | step rate con base instance | Otro | **Sí, limpio** tras #5260 | S | — |
| `7721a1ef` (`1cc85e39`) | #5260 · TheTurtle | Unbias de instance id en todos los usos | Otro (instancias corridas) | **A mano** (2 hunks de translate.cpp rechazados; tenemos el fix de literales 64-bit cerca). Upstream lo completó con **#5348 `643c7997`** | S | — |
| `945dbc3c` (`ca46b8b8`) | #5285 · Claudio Godoi | Barreras de subgrupo entre write/read LDS del mismo bloque | **Sombras** (grano del piso del octágono en AMD/RDNA), otro | **Sí**, solo falla el bump de versión en vk_pipeline_serialization.cpp | S | **Se superpone con AMD 0008** (ver comparación) |
| `ca838c58`, `914c5a1d`, `55e14c6b` | #5254 ajm MP3, #5317 address_space Windows, #5340 analógicos | — | — | **No aplica**: no vendorizamos ajm, address_space ni input | — | — |

### Commits del fork que no aplican (emulador/launcher)

`be60f09e`, `d97144a1`/`fa3c3292`/`12739d23` (memory_patcher), `1119359c`, `b89f36fb`, `ef844c9c`
(Readback Speed, region_manager de Shadlix), `15b0af1d` (FPS limiter del videoout HLE), `7421a242`
(skip intro: ya lo tenemos, `12522c5`), `63f4890e`/`a971a349` (PKG), `428ad3be` (input),
`6750a4c9`/`1a54440a`/`d5cc667e`/`b7285a7c`/`f0038b96` (Qt/config), `75879729`/`ed80f53c`
(signals Windows), `2c718d4f`, `a9c1f99a`, `a3ee89aa`, `2f33dd50` y los README/CHANGELOG.
Idea rescatable: `12739d23` parchea en UFC 3 1.14 EU (el mismo eboot que usamos) el par
1600×900 del `cmovne` (patrón `45 85 e4 b9 80 07 00 00 bf 40 06 00 00 ba 84 03 00 00 0f 45 f9`,
ancho en +9, alto en +14) para renderizar a cualquier resolución; podría ir a nuestro sistema de
parches como alternativa a `BB_NEO` (1080p) para 1440p/4K. Autor: mancelli.

## Solapamientos con los parches AMD

### Orden LDS: AMD 0008 vs #5285 (+ extensión de `8034718c`)

- **AMD 0008** (lucas): `OpMemoryBarrier(Subgroup, AcqRel|Workgroup)` antes y después de **cada**
  store LDS, en el backend, en todos los shaders y etapas. Probado: arregla el scan del piso
  (cs `3bb24942`) en RX 9070. Simple, cubre dependencias entre bloques, pero emite 2 barreras por
  store aunque no haya lecturas después.
- **#5285** (upstream): pasada IR que inserta la barrera de subgrupo solo entre transiciones
  write→read / read→write dentro de cada bloque y al final del bloque, solo en compute con LDS y
  cuando no corre la ruta de barreras de workgroup. Menos barreras, mismo modelo de memoria
  (`OpMemoryBarrier` Subgroup AcqRel|Workgroup, idéntico al de AMD 0008).
  Hueco: en el camino con `needs_lds_barriers` (NVIDIA/KosmicKrisp) los bloques **dentro de
  control de flujo divergente** no reciben nada; `8034718c` lo cierra con barreras de subgrupo
  ahí también.
- `8034718c` además cambia por defecto a `OpControlBarrier(Subgroup, …)`. **No tomar eso**: una
  barrera de control dentro de flujo divergente es comportamiento indefinido salvo que todo el
  subgrupo la alcance; mancelli lo necesitó para Intel SIMD16.

**Recomendación**: quedarse con **#5285 + la rama divergente de `8034718c` con barrera de
memoria** (no de control), conservar el interruptor `BB_LDS_ORDER=0` del parche AMD y retirar
AMD 0008. Así seguimos el código de upstream (próximos backports sin conflicto) con menos
barreras. Condición: el tester AMD vuelve a mirar el piso en la RX 9070 y alguien mira el Deck. Si
el piso vuelve a tener grano, revertir a AMD 0008 (que es superconjunto). Ambos suben
`ShaderBinaryVersion`: dejarlo en un solo bump.

### CMask: AMD 0011 vs `630b4e70` (vs upstream #5046)

- **AMD 0011**: en `SynchronizeMemoryFromImage` (lecturas texel de solo lectura) un CMask
  registrado se llena con 0xFFFFFFFF, igual que HTile con 0xf. Es el lugar natural del diseño y
  ya está probado en UFC 3 (modo Pro deja de ser pantalla negra).
- **`630b4e70`**: llena en `BindBuffers` cualquier binding de solo lectura cuya base es un CMask
  (también buffers no-texel). Cubre más casos, pero mete un FillBuffer en el camino caliente de
  bindings y duplica la lógica.
- **Upstream #5046 `995e37e4`** (TheThunderTurner, 11 oct, no está en el fork): lo mismo que AMD
  0011, pero con el valor correcto según muestras (1x/8x 0xFFFFFFFF, 2x 0xDDDDDDDD,
  4x 0xEEEEEEEE) y validando que la imagen dueña siga viva (`GetMetaInfo`, owner uid,
  Register/UnregisterMeta). AMD 0011 y `630b4e70` escriben 0xFFFFFFFF también para MSAA.

**Recomendación**: **quedarse con AMD 0011**, no portar `630b4e70`. Más adelante reemplazarlo por
#5046 cuando toquemos el registro de metadatos del texture cache (ya cambiado por nosotros). Solo
si aparece un shader de UFC 3 que lea CMask por buffer no-texel (log "Unhandled metadata type"),
extender la condición de AMD 0011 a `!is_written` sin importar texel. Eso ya es lo que hace
`630b4e70`.

### Otros cruces

- AMD 0003 (vista 2D por capa en DepthAsR16) ≈ parte de `72251298`: quedarse con lo nuestro + 0003.
- AMD 0004 (`BB_Z16S8_AS_D16`) rompe la suposición de `c08218db` (ver tabla).
- AMD 0017 `BB_GPU_WRITE_OVERLAP` y 0006 `BB_DEPTH_COPY_BUFFER` (descartables según su autor)
  tocan las mismas zonas que `01b32f45`; conviene no aplicarlos para que el port de T6 sea limpio.
- Los defaults de AMD (`BB_WRITE_WINDOW=0`, `BB_PREUPLOAD=0`, `BB_WRITE_STEP=0`) son
  **prerrequisito** para T6: el flush antes de escrituras CPU solo funciona si la escritura CPU
  hace fault.

## Upstream reciente que tampoco trae el fork y conviene mirar

#5283 FMASK fast clear (`14ada48a`, 1 línea), #5046 CMask (`995e37e4`), #5348 unbias en todo
indirect draw (`643c7997`), #5229 subrecursos agregados en overlap de depth (`c679ce60`), #5261
copias D32↔D32S8 por buffer (`92c7a884`), #5296 plano de stencil en DepthStencilCopy
(`69b6041d`), #5368 CopyImageWithBuffer todos los mips (`b3d46e96`), #5264 early fragment test
(`7d28d279`). Todos de video_core/recompilador; no se probaron contra nuestro árbol en este
ticket.

## Orden propuesto de backport (tickets)

Cada ticket con un único bump de versiones de caché de shaders donde corresponda, autoría
original en el mensaje (`Backport of <hash> by <autor>`, conservando sus `Co-Authored-By`) y una
línea en `gpu/VENDOR.txt`.

1. **T1 — Recompilador upstream (S, ~½ día).** #5220, #5346, #5197, #5274, #5260 (a mano) y
   #5348 opcional, #5257 + `5dedb48f` (mancelli) + `OpNClamp` en sirit + `02d3b06d` (mancelli,
   rehecho sobre #5257). Archivos: `shader_recompiler/**`, `liverpool_to_vk.*`,
   `vk_graphics_pipeline.cpp`, unas líneas de `vk_pipeline_cache.cpp`, `gpu/third_party/sirit`.
   Sin choque con los parches AMD. Prueba: pelea rápida + CF en Deck y AMD; comparar capturas.
2. **T2 — Orden LDS (S).** #5285 + rama divergente de `8034718c` con barrera de memoria;
   retirar AMD 0008 conservando `BB_LDS_ORDER`. Archivos: `shared_memory_barrier_pass.cpp`,
   `emit_spirv_barriers.cpp`, `ir_emitter.*`, `opcodes.inc`, `microinstruction.cpp`. Prueba: piso
   del octágono en RX 9070 (tester AMD) y Deck.
3. **T3 — Metadatos de color (S).** #5283 + la parte "MSAA nunca dibujado" de `789551b0`; dejar
   AMD 0011 para CMask (y #5046 a futuro). Archivos: `vk_rasterizer.cpp` (FilterDraw,
   EliminateFastClear), `buffer_cache.cpp`. Prueba: CF con/sin tatuajes, cambio de peleador.
4. **T4 — Retrato de career (S; primero experimento sin código).** Correr career con
   `BB_READBACK_LINEAR=1`; si aparece el retrato, portar el filtro de `e565b664` (2D lineal
   ≥64×64, no depth, no BC) como default. Archivo: `texture_cache.cpp` (FindTexture). Si no
   alcanza: logear el formato/tiling de la imagen en `cap_*` (el tester AMD sospecha imagen tiled)
   y pasa a T6.
5. **T5 — Depth (S).** `c08218db` con factor según el formato host real (×1 con
   `BB_Z16S8_AS_D16`), + D32→R32_UINT y liberación de copias de `72251298` sobre nuestro
   DepthAsR16. Archivos: `vk_rasterizer.cpp`, `host_shaders/` (shader R32). Prueba: sombras de
   peleadores en NVIDIA (D24S8/D32S8) y AMD.
6. **T6 — Piel del created fighter (L, dividir).** Requisito: defaults de write windows de AMD.
   - T6a (M): `ResolveOverlap` por memoria para vistas BC + write back de storage aliases antes de
     crear BC (`01b32f45` partes 1, 2 y 4), sin tocar page_manager. Antes, una línea de log en
     `ResolveOverlap` para confirmar que UFC 3 comprime BC en GPU al cambiar el tono.
   - T6b (M–L): flush de imágenes GPU antes de escrituras CPU (`01b32f45` parte 3), integrado con
     nuestro page_manager/write windows/write step/guest in place. Es el más riesgoso.
   - T6c (M): write back perezoso (`5b0e9954`) si T6a mete stutter en CF.
   Archivos: `texture_cache.cpp/.h`, `image.h`, `page_manager.cpp`, `vk_rasterizer.cpp`
   (InvalidateMemory).
7. **T7 — Pelo (M).** Primero comprobar que UFC 3 pone DB_ALPHA_TO_MASK (0x2DC bit 0) en los
   draws de pelo (log por pgm_hash); si sí, `69ab1996`. Archivos: `export.cpp`, `runtime_info.h`,
   `vk_pipeline_cache.cpp`, `vk_graphics_pipeline.*`, `vk_pipeline_serialization.cpp`. Pedir al
   usuario de GitHub una captura de qué ve exactamente en el pelo.
8. **Opcionales, según demanda:** guard de NaN / workgroups 1D para NVIDIA (`8034718c` +
   `731a29e7`) si un usuario NVIDIA reporta cuadros negros; `VkPipelineCache` persistente
   (`e669792b`); diagnóstico de "fault storm" (`84c6ede1`) para los cuelgues de write watchers;
   parche de resolución de UFC 3 (`12739d23`) en nuestro sistema de parches.
