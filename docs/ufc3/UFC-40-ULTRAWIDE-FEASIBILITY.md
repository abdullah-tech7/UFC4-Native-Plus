# UFC-40 · Factibilidad de ultrawide 21:9 en UFC 3

Ticket UFC-40, Claude 3, 16 oct 2026. **Segunda pasada**: integra la prueba try-1b de UFC-38 (§1.1) y
corrige las etiquetas de composite "Pro/base" y los bloqueos que try-1b ya resolvió.
**Solo investigación estática y documentación.** No se ejecutó el
juego, no se compiló, no hubo GDB, no se crearon parches, XML, scripts ni configs, y no se tocaron
partidas. No hay nada implementado. **No hay ningún resultado 21:9 medido ni observado.**

Formato confirmado por el usuario: **21:9**. La resolución exacta del monitor **no está confirmada**:
2560×1080 (64:27 ≈ 2,370) y 3440×1440 (43:18 ≈ 2,389) se usan solo como ejemplos. 32:9 queda fuera.

Base: anexo UFC-38 (`docs/ufc3/UFC-38-SUPERSAMPLING-TRIES.md`, tercera pasada `3885943`, que integra
try-1b) y la evidencia durable de try-1b en `/home/myzk/Projects/ufc3-native/logs/try1b-pro-options/`
(repo principal, no versionado; §1.1). Los archivos de `/tmp` de la primera pasada ya no existen y no se
enlazan.

Convenciones: **V** = releído por mí en esta pasada (bytes del eboot, SPIR-V, código del repo) ·
**D** = documentado por otro ticket · **I** = inferencia razonada · **H** = hipótesis · **NM** = no medido.

Direcciones del eboot: vaddr del guest (sin la base `0x800000000`); offset de archivo = vaddr + `0x4000`
(`out/eboot.elf`, SHA-256 `08a4ffbc…c18f`, el mismo de las Pruebas 1–3 según UFC-38).

---

## 0. Respuesta corta

- **Hoy, sin cambios, 21:9 se ve como 16:9 con barras laterales (pillarbox), sin estirar ni recortar**
  (V, código del presenter). Es el comportamiento correcto como punto de partida, no es "soporte ultrawide".
- **Un 21:9 real (más campo de visión a los lados) requiere que el juego guest renderice a 21:9.** El
  host no puede inventar los píxeles laterales: la escena llega ya rasterizada a 16:9 (I fuerte).
- Para eso hacen falta **al menos tres cosas del guest, ninguna resuelta**:
  1. tamaños de escena/UI/display más anchos. **try-1b demostró que el motor ya cambia el tamaño de la
     escena sin parches** (VIDEO → RESOLUTION → *Favor Resolution*: escena 3200×1800 → salida 1080, §1.1),
     así que "¿puede el juego renderizar un RT mayor?" ya no es un bloqueo. Lo pendiente es el **mapeo
     estático campo → RT para el ancho** (qué campo es escena, cuál UI/salida) y forzar un ancho 21:9;
  2. el **aspect de proyección** de la cámara: hay varios defaults **constantes 16:9** en el código de
     cámara (§3), así que agrandar solo los RT probablemente **estira** la escena (H fuerte);
  3. HUD/menús Scaleform: el modo de escalado del stage es desconocido (§5).
- **Viable en principio, no garantizado.** Es un trabajo de parche guest por firma más ajustes portables
  menores en el host. Ninguna parte es trivial y no se promete HUD ni FOV correctos sin arreglos.
- El pillarbox de hoy es **lo esperado por código**, no una prueba manual 21:9 hecha (§9). En esta pasada
  no se pide ninguna prueba; lo siguiente es trabajo estático priorizado (§10).
- Resolución exacta del monitor: sigue pendiente; no detiene el análisis (se diseña para 2560×1080).

---

## 1. Qué se reutiliza de la investigación de supersampling (UFC-31/36/38) y qué es nuevo

| Elemento | De UFC-31/36/38 | Uso para 21:9 |
|---|---|---|
| Selector `0x533180` y sus campos S/desc | D/V | **Reutilizable.** Mismo punto de entrada; pero en 21:9 cambia **solo el ancho** (alto 1080 se mantiene en 2560×1080) |
| **Cadena 1800 observada en try-1b** (escena 3200×1800, UI/salida 1920×1080, composite `e5b29094`) | V (captura) | **Modelo de referencia**: muestra qué RT siguen el tamaño de escena y cuáles quedan atados a la salida (§1.1, §2.2) |
| Modo "high"/"ultra" (`0x428b90`) ↔ *Favor Performance*/*Favor Resolution*, sets `VideoOutTex_High/Low` | D/H | Ya **no es bloqueo de existencia**: el set High se activa desde el menú. El mapeo opción → modo → setter sigue H (UFC-38 §2) |
| Infraestructura de parches por firma (`scripts/patches.py`, `run.sh:119`) | V | **Reutilizable** sin cambios |
| Composites `ddbe162b` (`texelFetch`, escena = salida) / `e5b29094` (`textureLod` escena + `texelFetch` UI, escena ≠ salida) | V | **Corregido**: no son "Pro"/"base"; `e5b29094` actúa en Neo 1 con *Favor Resolution* (§4) |
| Presenter host: escalado lineal de un tap, FSR1 solo si sube | V | Reutilizable; en 21:9 el presenter ya respeta aspect (§6) |
| Estimación de memoria de RT (≈116 MB a 1080) | D/H | Escala lineal con píxeles; estimación, no VRAM medida (§8) |
| **Aspect de proyección / FOV de cámaras** | — | **Nuevo** (§3) |
| **Modo de escala del stage Scaleform, safe zone, FMV rect** | — | **Nuevo** (§5) |
| **Hardcodes 1920×1080 del host** (UI composition, temporal) | parcialmente | **Nuevo para 21:9** (§6.3) |

Lo que **no** se reutiliza: los offsets o parches de CTR (otro juego), el sitio de mancelli (sin efecto en
Pro, UFC-38), y la idea "cambiar swapchain/ventana basta" (descartada en §6).

### 1.1 Evidencia nueva: try-1b (V, releída en esta pasada)

Fuente durable: `logs/try1b-pro-options/capture/frame_1792163000.txt` del repo principal, SHA-256
`62833f1ea7673759b345b1daf9dc35872c84a63b86cf1a4f6c740c972474fb26` (V, `sha256sum` coincide), más
`menu-favor-resolution.png` y `ring-favor-resolution.png`. Detalle completo en UFC-38 §0.1/§1.

- Config: `BB_NEO=1`, VideoOut 3840×2160, ventana 1920×1080, límite 60, `BB_WRITE_STEP=0`, write windows y
  preupload 0, **0 parches**. Opción del juego elegida por el usuario: **VIDEO → RESOLUTION → *Favor
  Resolution*** (la otra es *Favor Performance*).
- Captura de 198 pases: escena B10G11R11 y depth D32S8 a **3200×1800**, tonemap RGBA8 `0x109ffe0000`
  3200×1800, UI/HUD RGBA8 `0x109eee0000` **1920×1080**. Pase final:

```
#197 COMPUTE dispatches 1, cs 00000000e5b29094
  samples 0x109eee0000 R8G8B8A8Unorm 1920x1080      ← UI
  samples 0x109ffe0000 R8G8B8A8Unorm 3200x1800      ← escena tonemapeada
  writes  0x109a470000 R8G8B8A8Srgb 1920x1080       ← presented buffer
  note: groups 240x135x1
```

**Confirmado:** un frame de combate con escena interna 1800 reducida por el propio juego a 1080, sin
parches. **No confirmado:** 4K nativo (no hay ningún target 3840×2160), rendimiento (no hubo benchmark),
estabilidad, control con *Favor Performance* (no se hizo) ni el binding exacto opción → variables de G
(no hay traza). El screenshot es solo de la ventana y muestra el HUD.

**Qué aporta a 21:9 (I):**
- La escena puede tener un tamaño distinto de la UI y de la salida dentro del mismo frame; la UI y el
  buffer presentado quedan atados al tamaño de **salida** (1920×1080), no al de escena. Para 21:9 hay que
  ensanchar **las dos** familias: escena y UI/salida.
- Los RT derivados de la escena escalan proporcionalmente (960→1600, 96→160, UFC-38 §1.1) y el linear
  depth 1600×900 se mantiene. Refuerza que los derivados siguen a `width0` y señala que hay RT de forma
  fija a vigilar.
- La correspondencia campo ↔ RT queda sugerida por la rama 1800 del selector (UFC-38 §2):
  `S+0x38/3c` y `desc+0x5c/60` = 3200×1800 ↔ escena; `S+0x40/44` = 1600×900 ↔ linear depth;
  `S+0x48..0x5c`, `desc+0x78/7c`, `desc+0x90/94` = 1920×1080 ↔ UI/salida. **I, no trazado.**

---

## 2. Tamaño real de escena/RT (capa guest)

### 2.1 Qué fija el selector (V, desensamblado de `0x533180` releído)

Ruta normal (no 1800), tras `test r12d,r12d` (r12d = Neo):

```
0x53325a  b9 80 07 00 00   mov ecx,1920      ; Pro: ancho de render
0x53325f  bf 40 06 00 00   mov edi,1600      ; base
0x533264  ba 84 03 00 00   mov edx,900
0x533269  0f 45 f9         cmovne edi,ecx    ; Neo → 1920
0x53326c  b9 38 04 00 00   mov ecx,1080
0x533271  0f 45 d1         cmovne edx,ecx    ; Neo → 1080
→ S+0x38/3c, +0x40/44, +0x48/4c, +0x50/54 = render (1920×1080 Pro)
0x5332a0  movl $0x780,0x58(%r14)  ; S+0x58 = 1920  (fijo, también en la rama 1800)
0x5332a8  movl $0x438,0x5c(%r14)  ; S+0x5c = 1080  (fijo)
desc+0x5c/60 = render · desc+0x90/94 = render
0x5332d1  movl $0x780,0x78(%r15)  ; desc+0x78 = 1920 (fijo)
0x5332d9  movl $0x438,0x7c(%r15)  ; desc+0x7c = 1080 (fijo)
```

- Para 21:9 a 2560×1080, en la ruta Pro bastaría cambiar el **ancho** de render (imm `0x53325b`) y los
  **anchos fijos** `S+0x58` (imm `0x5332a4`) y `desc+0x78` (imm `0x5332d5`). El alto 1080 no cambia (I).
  Son 3 inmediatos de 32 bits, no 2 como en 2A. **Qué consume cada uno no está identificado**: UFC-36
  dejó sin resolver los consumidores de S+0x38.. y quién escribe G+0x4c..0x70 (D).
- La consulta `sceVideoOutGetResolutionStatus` (`video_out.cpp:302`) devuelve `BB_INTERNAL_WIDTH/HEIGHT`
  (`emulator_settings.h:23–24`, `video_out.cpp:495`), pero el juego solo compara el alto con 2160
  (`0x53320a: cmpl $0x870`, V; UFC-31 D). **`BB_INTERNAL_WIDTH=2560` por sí solo no cambia nada** (I fuerte).

### 2.2 El pipeline de render está parametrizado por variables con nombre (V cadenas, I uso)

El eboot tiene un intérprete de script de render (`settarget, settexture, setview, setdepth, setviewport,
resolvetarget…`, `0x1e1ba61`) y variables `width0, height0, halfwidth0, quarterwidth0, offscreenwidth0,
lowres_width0, lowres_offscreenwidth0` (`0x1e07e40`) más derivadas `*_div_`/`*_mul_` (`0x1e8d000`).
Es el singleton G de UFC-36.

- **I:** si `width0` (o `lowres_width0` en modo "high") vale 2560, los RT derivados (half, quarter,
  bloom) deberían seguirlo sin tocar cada uno. Eso es favorable para 21:9.
- **H/riesgo:** RT de forma fija observados (960×1080, 96×1080, 1600×900 lineal, D) pueden no derivar de
  `width0`; quedarían en 16:9 y desalinearían efectos.
- `setviewport` es comando de consola/script (V cadena `0x1e1983c`); no se buscó si el script de UFC usa
  viewports absolutos.

### 2.3 Estado

| Capa | Estado |
|---|---|
| Sitios de inmediatos del selector para ancho | **V** (bytes) |
| Que el motor renderice la escena a un tamaño distinto de 1080 sin parches | **V** (try-1b, un frame a 3200×1800) |
| Que cambiar esos inmediatos cambie la escena a 2560 | **H** (falta el mapeo campo → RT; con modo "high" la escena sale del set Low) |
| Que los RT derivados escalen solos | **I** (reforzado: escalan en la cadena 1800) |
| Heap/memoria suficiente | **NM** (§8) |

---

## 3. Proyección de cámara: aspect ratio y FOV (capa guest, lo más nuevo)

Hallazgos (V, cadenas y desensamblado puntual; no RE exhaustivo):

| Sitio | Qué es | Evidencia |
|---|---|---|
| `0x4932a0..0x4932d0` | 4 getters virtuales consecutivos que devuelven constantes de `.rodata` `0x1e83b48..`: **0,6109 rad (35°)**, **1,7778 (16:9)**, 0,35, 650 | V. Interpretación (H): defaults de cámara FOV/aspect/near/far |
| `0xc649e9` (+`0xc64aa4`, `0xc64b07`, `0xc64b80`) | **CameraView RigOp** (cadenas `CameraRigFeature`, `CameraViewBinding`): parámetros con default **16:9**, **55,0**, 1,0, 1000,0; cada uno se reemplaza si hay binding (patrón `call *0x18` / `0xd524b0`) | V. Interpretación (H): aspect, FOV°, near, far con aspect por defecto constante |
| `0x7bab10` (tabla `0x1eb0938`) / `0x7ba940` | Nodo ANT "compute look at and fov given camera position and a cloud of points to frame": campos `CameraPos`, **`AspectRatio` (default 16:9 en `0x7bab47`)**, `Vertical/HorizontalFramingCorrection`, `AdditionalHorizontalFovInDegrees`, `Point0..7` | V |
| `0xd10fc1..0xd11041` | Asset de cámara animada: `AspectRatioAsset`, **`ForcedAspectRatio`**, **`FovIsHorizontal`**, `FovInDegrees`, `ProjectionMatrixAsset`… | V cadenas |
| `0x1146795..0x1146c9b` | Cámara de combate (framing): **`CameraVerticalFOV`**, `CameraVerticalFOVMin/Max`, `TargetAScreenLocation`, `ScreenSpaceTolerance`, `FOVSlider` | V cadenas |
| `0x29397c` | Ctor de un objeto de escena "Noa Scene" escribe `+0x8c = 0.0`, `+0x90 = 16/9` | V; uso desconocido |
| Cadena `LETTERBOX`/`Letterbox` (`0x1fcf888`) | Parámetro de pista de cámara cinemática junto a DOF/shake | V; las cinemáticas pueden dibujar sus propias barras |

Consecuencias:
- **H fuerte:** al menos parte del aspect de proyección **no se deriva del tamaño del RT**, sino de
  defaults constantes 16:9 sobreescribibles por bindings. Si el juego no alimenta un aspect vivo, agrandar
  los RT a 2560×1080 **estira la escena horizontalmente** (personas más anchas). Ese sería el modo de falla
  esperado de un parche que solo toque tamaños.
- **I favorable:** la cámara de combate trabaja con **FOV vertical** (`CameraVerticalFOV`). Con FOV
  vertical fijo, si el aspect se corrige, el resultado es **Hor+** (más campo a los lados, mismo alto):
  `hfov = 2·atan(tan(vfov/2) · aspect)`. No se conoce el vfov real de UFC en combate; 35° y 55° son
  defaults, no valores medidos.
- **Riesgo Vert-/recorte en cinemáticas:** las cámaras animadas pueden tener `FovIsHorizontal` y
  `ForcedAspectRatio`. Con FOV horizontal fijo, un aspect más ancho **recorta arriba/abajo** (zoom). El
  valor por asset es desconocido; los assets están comprimidos (`.big` con cabecera `EB\0\3`, UFC-38).
- **Framing de combate:** el nodo de framing usa `AspectRatio` (default 16:9) para encuadrar a los dos
  peleadores. Si queda en 16:9 con una pantalla 21:9, el encuadre se calcula para 16:9 y el borde extra es
  "gratis" (I). Si se le pasa 21:9, la cámara podría acercarse más (`AllowCameraToFrameCloserThanAuthored`
  existe como cadena). **No se sabe cuál de los dos ocurriría.**
- **Parche guest por firma para el aspect**: candidato a investigar, no a aplicar: las constantes 16:9 de
  `0x1e83b4c` (compartida por el getter `0x4932b0` y por el RigOp `0xc649e9`) y el inmediato de
  `0x7bab47`. Cambiar una constante de `.rodata` compartida puede afectar a otros consumidores; antes hay
  que listar todas las referencias (hay 3 coincidencias de `39 8e e3 3f` en el eboot; se identificaron las
  tres) y cualquier otro cálculo `w/h`.

### Estado

| Capa | Estado |
|---|---|
| Existen defaults 16:9 en código de cámara | **V** |
| La proyección en combate usa esos defaults (no aspect vivo) | **H** |
| Combate con FOV vertical → Hor+ si se corrige aspect | **I** |
| Comportamiento en cinemáticas/replays | **H** (FovIsHorizontal/ForcedAspectRatio por asset, desconocido) |

---

## 4. Viewport/scissor y composite UI/escena (capa guest + shaders)

### 4.1 Composite `ddbe162b` (escena = salida; V, `spirv-cross` de `user/cache/CUSA06534/0x00000000ddbe162b_0.spv`)

- Compute 8×8; lee escena (`img0`) y UI (`img1`) con **`texelFetch` en la misma coordenada** que escribe
  la salida (`img2`), y consulta una **máscara de cobertura de UI** en buffers con direccionamiento por
  tiles cuyo pitch sale de un descriptor (`(data & 4095) + 63 >> 6`).
- **Reinterpretación para 21:9:** `texelFetch` no recorta si **escena, UI, máscara y salida tienen el
  mismo tamaño** (todas 2560×1080). El recorte de 2A aparecía porque escena y salida tenían tamaños
  distintos. En 21:9 el riesgo es otro: si la **UI o la máscara quedan en 1920** y la salida pasa a 2560,
  la UI ocupa los 1920 de la izquierda y la franja derecha lee fuera de rango (I).
- El tamaño de dispatch y los descriptores los arma el guest; no hay que tocar el shader si el guest crea
  todo coherente (I).

### 4.2 Composite `e5b29094` (V SPIR-V UFC-38; V captura try-1b)

**Corrección:** la primera pasada lo llamó "composite base" (PS4). Es inexacto: en try-1b corre con
`PS4 Pro mode: 1` y *Favor Resolution*. Lo que sostiene la evidencia (I, UFC-38 §1.2) es que el motor
elige el composite según la relación escena/salida: `ddbe162b` cuando la escena es del tamaño de la
salida, `e5b29094` cuando no.

- Lee la escena con `textureLod` y **coordenada normalizada** y la UI con `texelFetch`; el dispatch sale
  del tamaño de salida (240×135 grupos de 8×8 = 1920×1080).
- Consecuencia para 21:9 (I): la coordenada normalizada mapea **toda** la escena sobre **toda** la salida.
  Una escena 21:9 compuesta en una salida 16:9 se **comprime** horizontalmente (y al revés estira). Con
  esta ruta no hay recorte, pero el aspect de escena y salida tiene que coincidir. Además, la UI se lee
  1:1, así que con salida 2560 la UI también tiene que medir 2560 (si no, misma franja que §4.1).
- Es la ruta que permitiría 21:9 **con** supersampling (escena mayor que la salida); para un primer
  intento 2560×1080 sin supersampling se espera `ddbe162b` (H).

### 4.3 Viewport/scissor

No hay evidencia capturada de viewports/scissors de UFC: la captura disponible
(`out/capture/frame_1791947138.txt`, base 1600×900) lista RT, formatos y draws, pero no viewports (V).
Lo que se sabe de viewports viene del código del host (§6.3). **NM.**

---

## 5. HUD, menús, cinemáticas, replays, career (capa guest)

| Contenido | Evidencia | Expectativa 21:9 |
|---|---|---|
| HUD/menús Scaleform/Flash | V cadenas: `StageScaleMode`, `showAll`, `noBorder`, `exactFit`, `noScale`, `scaleMode`; `Stage::stageWidthSet()/fullScreenWidthGet() is not implemented`; `Capabilities::screenResolutionX/YGet() not implemented` | Depende del modo usado: `showAll` → UI 16:9 centrada con márgenes (deseable); `noBorder` → recorte arriba/abajo; `exactFit` → **estirada**; `noScale` → anclada a una esquina. **Desconocido (H).** El tamaño del stage puede estar fijo a 1920×1080 |
| Safe zone | V: `graphics/render/render-safe-zone-mask`, `drawSafeZoneMask` (debug) | Sin relación directa con aspect (I) |
| Videos (FMV) | V: variables `FMVx/FMVy/FMVw/FMVh` del script de render | Rectángulo parametrizable; si se calcula para 1920 queda a la izquierda o se estira (H) |
| Cinemáticas (walkouts, presentaciones) | V: pista `LETTERBOX`, `FovIsHorizontal`, `ForcedAspectRatio` | Riesgo de recorte o barras propias (§3) |
| Replays | V: `Choose Replay Camera`, `ReplayChoice` (`0x7ba700`) | Mismo sistema de rigs que combate; sin evidencia propia |
| Career / selección de peleador | Retratos con tamaños fijos (§6.3) | Retratos de alto 1080 no cambian en 2560×1080 (I) |

**No se promete HUD correcto.** El escenario más probable sin arreglos específicos es HUD a la izquierda o
estirado (H).

---

## 6. Presentación en el host (capa portable)

### 6.1 Lo que hace hoy (V)

- `vk_presenter.cpp:397`: `expected_ratio = ancho/alto` del buffer del guest.
- `vk_presenter.cpp:777–790` (`SetExpectedGameSize`): el frame intermedio se crea con el aspect del juego
  dentro del tamaño de la swapchain.
- `vk_presenter.cpp:641–652`: limpia la swapchain a negro y hace `blitImage` con `MakeImageBlitFit`
  (`:97–122`, centra con offsets), filtro lineal. **Resultado: pillarbox 16:9 en una ventana 21:9, sin
  estirar ni recortar.** Existe `MakeImageBlitStretch` (`:92`) pero no se usa.
- Ventana: `gpu/shim/window.cpp:74–80` acepta cualquier `BB_WINDOW_SIZE=WxH` entre 320×200 y 7680×4320
  (2560×1080 y 3440×1440 incluidos). `BB_FULLSCREEN=1` toma el tamaño del monitor. El launcher de Windows
  solo ofrece `1920x1080/2560x1440/3840x2160` (`sacredfire_launcher.c:61`); el de Linux no fija tamaño.
- Driver VideoOut (`driver.cpp:180–230`): exige `aspect_ratio == 0` (el único valor del SDK, 16:9 de la
  consola) pero **no limita el ancho**, solo `width <= pitch`. Un buffer de display 2560×1080 se aceptaría
  (V). Con eso el presenter mostraría 21:9 sin barras automáticamente (I).

### 6.2 Por qué cambiar swapchain/ventana no basta

El guest compone y entrega un buffer 16:9. El host solo puede pillarbox (correcto), estirar (deforma) o
recortar arriba/abajo (pierde imagen). **Ninguna opción del host da más campo de visión.** Un "fake
ultrawide" por extensión de bordes o reproyección no es aceptable para el criterio del ticket.

### 6.3 Hardcodes 1920×1080 del host que afectarían a un guest 21:9 (V)

| Sitio | Qué hace | Impacto 21:9 |
|---|---|---|
| `ui_composition.h:20–23` `NativeViewport` | Detecta viewports UI de 1920×1080 exacto | Solo se usa en la ruta escalada (redirect/temporal); con escala nativa no aplica (I). Habría que generalizarlo si se combina 21:9 con escalado |
| `ui_composition.h:33–37` `Scale` | Divide por 1920/1080 en coordenadas nativas | Ídem |
| `vk_temporal_upscaler.h:159`, `.cpp:202–203, 1406, 1411` | Asume guest 1920×1080 | Ruta temporal (forzada Off en UFC, D UFC-35); no bloquea un primer intento |
| `vk_scene_resolution.cpp:74–96` | Solo acepta 1920×1080 / 960×540 | try-3 ya descartada; no aplica |
| `vk_rasterizer.cpp:4056` | Factor relativo a 1920×1080 | Solo con `reduced`, que no se activa en UFC (D UFC-38) |
| `texture_cache.cpp:106–116` `T4Portrait` | Tamaños exactos de retratos (652×1080, 600×1080…) | Con alto 1080 no deberían cambiar (I); a 3440×1440 sí podrían |
| `vk_presenter.h:113–115` | Valores iniciales 1920×1080 | Se recalculan en cada frame (V); sin impacto |

**Fix portable posible (pequeño, no urgente):** generalizar `NativeViewport`/`Scale` para que la
resolución "nativa" de UI sea la del guest en vez de 1920×1080. Solo hace falta si se usa la ruta escalada.

---

## 7. Límites 21:9

- **Resolución del monitor sin confirmar.** 2560×1080 mantiene el alto 1080 (menos riesgo: retratos, RT de
  alto fijo, memoria +33 %). 3440×1440 cambia ancho **y** alto: combina el problema 21:9 con el problema
  1440 de UFC-38 (memoria, heap, retratos, `T4Portrait`). **Recomendación: diseñar para 2560×1080 interno**
  y, si el monitor es 3440×1440, presentar 2560×1080 escalado a la ventana (el presenter ya escala con
  aspect correcto). 64:27 y 43:18 difieren menos de 1 %: la imagen 2560×1080 en 3440×1440 deja ~9 px de
  barra por lado (I, cálculo).
- **Hor+ depende del aspect de proyección** (§3), no de los RT.
- **Cinemáticas pueden quedar recortadas o con barras propias** aunque el combate funcione.
- **HUD**: sin solución conocida; puede requerir parches de UI/Scaleform aparte.
- **Combinación con 1800:** no combinar en la misma prueba. La cadena 1800 de try-1b se usa como modelo
  estático (qué RT siguen a la escena), no como objetivo; 21:9 a 1800 (4267×1800) no se considera.
- **Online/determinismo:** el render no debería afectar la simulación, pero la cámara de combate y su
  framing **podrían** alimentar lógica (p. ej. visibilidad, LOD o selección de cámaras de replay). **No se
  asume seguro ni que cause desync.** 21:9 se valida solo offline; online queda deshabilitado para 21:9
  hasta tener evidencia, y ambos pares no tienen por qué usar el mismo aspect si se demuestra que es solo
  visual (H).

---

## 8. Riesgo de rendimiento y memoria (NM; estimación)

| Resolución interna | Píxeles | Relación vs 1920×1080 | RT estimados (escala lineal sobre ≈116 MB D/H) |
|---|---|---|---|
| 1920×1080 | 2,07 M | 1,00 | ≈116 MB (D) |
| 2560×1080 | 2,76 M | 1,33 | ≈155 MB (H) |
| 3440×1440 | 4,95 M | 2,39 | ≈277 MB (H) |

El +33 % de 2560×1080 sobre 1080 es una **estimación por píxeles** de los RT dependientes de la
resolución, no una VRAM total medida (no se midió VRAM en ninguna prueba, tampoco en try-1b).
Más geometría visible a los lados (Hor+) suma draws/culling: coste de CPU/GPU **NM**. El baseline de
Test 1 (60,00 FPS, 1 % low 46,72, D) es la referencia; ningún número 21:9 está medido. Heap del motor
desconocido (UFC-38).

---

## 9. Pruebas manuales: ninguna pedida en esta pasada

- El pillarbox 16:9 en ventana 21:9 (§6.1) es el **comportamiento esperado por código**. **No hay ninguna
  prueba manual 21:9 hecha.**
- No se propone una batería de pruebas. Si más adelante el Jefe quiere un control, el candidato único es
  **T21-0**: misma config que Test 1 (Neo 1, VideoOut 1080, sin parches, *Favor Performance*), una corrida
  con ventana 1920×1080 y otra con `BB_WINDOW_SIZE=2560x1080` como única variable, screenshot +
  `capture.trigger` en el ring. Éxito = imagen centrada con barras, sin estirar ni recortar, y captura con
  escena/UI/salida en 1920×1080. Ojo: la opción RESOLUTION persiste en la copia de `user/`; hay que
  anotar su valor.
- Cualquier parche de ancho (T21-1) va después del trabajo estático de §10.

---

## 10. Priorización del siguiente trabajo estático (actualizada con try-1b)

Todo es estático, reutiliza la cadena 1800 ya observada y no requiere parches ni pruebas.

| # | Trabajo | Qué responde | Por qué ahora | Archivos |
|---|---|---|---|---|
| 1 | **Mapeo de ancho campo → RT** | Qué campos del selector/G alimentan la escena (3200 en try-1b), la UI y el buffer presentado (1920), el linear depth (1600×900) y el dispatch del composite; consumidores de `S+0x38/0x58`, `desc+0x5c/0x78/0x90`, `VideoOutTex_High/Low` (`0x10d300 → 0x118b60`) | try-1b da etiquetas reales para cada tamaño; sin esto no hay sitio fiable para poner 2560 | solo `docs/` |
| 2 | **Aspect de cámara** | Todas las referencias a la constante 16:9 `0x1e83b4c`, el inmediato `0x7bab47` y cualquier cálculo `w/h` de RT; si la cámara de combate recibe aspect vivo o default | Decide si un ancho mayor da Hor+ o estira; es independiente de 1 | solo `docs/` |
| 3 | **HUD/UI** | Qué fija el tamaño del RT de UI (1920×1080 incluso con escena 1800), modo `StageScaleMode`, rect FMV | La UI está atada a la salida: si la salida pasa a 2560, la UI debe acompañar | solo `docs/` |
| 4 | **Composite** | Confirmar en el SPIR-V de `e5b29094`/`ddbe162b` si hay rect/offset/escala de UV además de la coordenada normalizada y cómo se elige uno u otro | Determina si el aspect escena/salida debe coincidir exactamente | solo `docs/` |
| 5 | Host portable (opcional) | Generalizar `UiComposition::NativeViewport/Scale`; opción 2560×1080 en el launcher | Solo con escalado; no bloquea | `ui_composition.h`, launchers |

Después de 1–3: un XML por firma para el ancho (T21-1) y otro **distinto** para el aspect, activables por
separado. Online: ninguno lo habilita; validar offline primero.

---

## 11. Estado por capa (resumen)

| Capa | Hoy | Para 21:9 real | Certeza |
|---|---|---|---|
| Ventana/swapchain | Cualquier WxH | Listo | V |
| Presentación | Pillarbox correcto | Si el guest entrega 21:9, se muestra sin barras | V / I |
| Driver VideoOut | Acepta buffers más anchos | Listo | V |
| Composite | `ddbe162b` (1:1) o `e5b29094` (escena normalizada) | OK si escena y salida tienen el mismo aspect y UI = salida | V / I |
| Tamaños de RT guest | 1920×1080 por defecto; escena 3200×1800 con *Favor Resolution* | Ancho 21:9 en escena **y** UI/salida; mapeo campo → RT pendiente | V sitios y cadena 1800 / H efecto del ancho |
| Proyección/FOV | Defaults 16:9 | Parche de aspect necesario (probable) | V defaults / H uso |
| HUD/menús | 16:9 | Desconocido | H |
| Cinemáticas/replays | 16:9 | Riesgo de recorte/barras | H |
| Memoria/rendimiento | baseline Test 1 | +33 % píxeles a 2560×1080 (estimación, no VRAM medida) | NM |
| Online | 16:9 | No habilitar sin evidencia | — |

---

## 12. Pendiente / datos que faltan

- Resolución exacta del monitor del usuario (no bloquea: el diseño apunta a 2560×1080 interno).
- try-1b: hecho un frame positivo (1800 → 1080). Siguen pendientes en UFC-38 el control *Favor
  Performance*, benchmark, estabilidad y el binding opción → modo → G. No bloquean el trabajo estático 21:9.
- Mapeo de ancho campo → RT, aspect de cámara y HUD (§10, puntos 1–3).
- Ninguna prueba manual 21:9 hecha; el pillarbox es esperado por código.
- Modo de escalado Scaleform y assets `.big` comprimidos sin herramienta de desempaquetado.
- Nada de esto está medido.
