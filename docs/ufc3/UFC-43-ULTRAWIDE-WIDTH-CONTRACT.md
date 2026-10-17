# UFC-43 · 21:9: contrato de ancho para escena, interfaz y salida

Ticket UFC-43, Claude 2, 16 oct 2026. Sigue a UFC-40 (`ae3ccdb:docs/ufc3/UFC-40-ULTRAWIDE-FEASIBILITY.md`)
y UFC-38 (rama `agents/ufc-38-…`, `docs/ufc3/UFC-38-SUPERSAMPLING-TRIES.md`). **Solo análisis estático y
documentación**: no se ejecutó el juego, no compilé, no usé GDB/ptrace, no creé parches, XML ni configs, y
no toqué el proceso que está corriendo. La cámara (FOV/aspect) la estudia Claude 3 en otro ticket y aquí
no se toca. No hay ningún resultado 21:9 medido ni observado.

Convenciones: **V** = releído por mí en esta pasada (bytes/desensamblado del eboot, texto de captura, log,
código del repo) · **D** = documentado por otro ticket · **I** = inferencia razonada · **H** = hipótesis ·
**NM** = no medido.

Direcciones: vaddr del guest sin la base `0x800000000`. Offset de archivo = vaddr + `0x4000`
(`out/eboot.elf` del repo principal, SHA-256 `08a4ffbc…c18f`, V). En runtime: `0x800000000 + vaddr`.

---

## 0. Respuesta corta

- El ancho **no** vive en un solo sitio, pero **sí sale de un solo selector** (`0x533180`) y se reparte por
  **tres caminos estáticos** (V): S → singleton de render G (escena y efectos), desc → texturas de la
  cadena de swap (escena final, overlay/UI, buffers de VideoOut) y S+0x58 → `WebKitSurface`.
- **La UI/HUD es el `RNA:OverlaySwapChainTex`, de tamaño `desc+0x78/0x7c`** (imm fijo 1920×1080 en
  `0x5332d1`). Eso explica por qué la UI mide 1920×1080 en las tres capturas (base 1600×900, *Performance*
  1080 y *Resolution* 1800) (I fuerte, §3).
- **`S+0x58/0x5c` (imm `0x5332a0`) no alimenta la UI**: llega a G+0x74/0x78 y su único consumidor estático
  es la textura `WebKitSurface` (V). Corrige el supuesto de UFC-40 de que era uno de los "3 inmediatos"
  necesarios.
- **El buffer presentado (VideoOut) mide `max(desc+0x90, desc+0x78)` de ancho** en las tres capturas (I
  fuerte; §3.3, con una contradicción estática abierta sobre el set "High", §4.2).
- La opción *Favor Performance / Favor Resolution* **no reescribe ningún ancho**: solo elige qué par de G
  usa el render (`G+0x90`) y qué `SwapChainTex` (High/Low) es la escena final (V, §4).
- **Ruta inicial elegida para un futuro 2560×1080: Pro (`BB_NEO=1`) + VideoOut a 1080 (rama normal del
  selector) + *Favor Performance***. En esa rama los cinco pares de render y `desc+0x5c/0x90` salen de
  **un solo registro** (`edi`, imm `0x53325b`), así que escena, offscreen, lowres, SwapChainTex y VideoOut
  pasan a 2560 juntos y el menú deja de importar para los tamaños. Hace falta además el overlay
  (`0x5332d5`) para que UI = salida. **Dos inmediatos obligatorios, uno opcional** (§5).
- **No basta con eso para una imagen correcta**: falta el aspect de cámara (otro ticket), no se sabe cómo
  dibuja Scaleform dentro de un overlay de 2560, el rect FMV está en un espacio virtual 1280×720 fijo y el
  tamaño del buffer de máscara del composite `ddbe162b` no está trazado (§6). Lo que sí se puede probar con
  esos dos inmediatos es la **coherencia de tamaños** de escena/UI/salida (§8).

---

## 1. Fuentes

| Fuente | Estado | Uso |
|---|---|---|
| `logs/try1b-pro-options/capture/frame_1792163000.txt` (repo principal) | V, SHA-256 `62833f1e…26fb26` | *Favor Resolution*: escena 3200×1800, composite `e5b29094`, UI 1920×1080 → presentado 1920×1080 |
| `logs/try1b-pro-options/capture/frame_1792163382.txt` | V, SHA-256 `066c85a2…4e8821` | *Favor Performance*: escena 1920×1080, composite `ddbe162b`, UI 1920×1080 → presentado 1920×1080 |
| Ambas capturas | D (Jefe/usuario) | Misma sesión: Neo 1, VideoOut 2160, ventana 1080, límite 60, 0 parches; el usuario cambió el menú en caliente. No repetidas |
| `out/capture/frame_1791947138.txt` | V, SHA-256 `b85c3f64…9c55fe` | Base (Neo 0): render 1600×900, UI 1920×1080, `e5b29094`, presentado 1920×1080 (tercer punto de control) |
| `out/run20.log`, `run42.log`, `run44.log` | V | Neo 0: `RegisterBuffers … bufferNum = 2, A8R8G8B8Srgb, width = 1920, height = 1080, pitch 1920` |
| `out/eboot.elf` | V | Desensamblado lineal completo del texto en `/tmp/ufc43/all.s` (no versionado) |

Los logs de try-1b no traen las líneas `Lib.VideoOut <Info>` (`RegisterBuffers`, `SetBufferAttribute`):
el filtro de log de esa corrida no las incluye (V, 0 coincidencias). Eso deja abierto el §4.2.

---

## 2. La cadena estática (V salvo indicación)

```
0x49f8e0 → call 0x533180 (selector; rsi = S, rdx = desc)          [0x49fab0]
  rama 1800 (Neo ∧ Open ∧ query ok ∧ alto == 2160):
    S+0x38/3c = 3200x1800  S+0x40/44 = 1600x900  S+0x48/4c = S+0x50/54 = 1920x1080 (ecx/eax)
  rama normal (Pro):  edi = 1920 (imm 0x53325b), edx = 1080
    S+0x38 = S+0x40 = S+0x48 = S+0x50 = edi ; alturas = edx
  ambas: S+0x58/5c = 1920x1080 (imm 0x5332a0/a8), S+0x64 = S+0x65 = 1, S+0x67 = rama1800
         desc+0x5c/60 = (r8d, edx)  = S+0x38/3c   fmt desc+0x58 = 0x23
         desc+0x78/7c = 1920x1080 (imm 0x5332d1/d9)  fmt desc+0x74 = 0x23
         desc+0x90/94 = (ecx, eax)  = S+0x48/4c   fmt desc+0x8c = 0x23
         desc+0x6c = 2 (nº de buffers; coincide con bufferNum = 2 del log)
  → 0x10d300(obj, desc): copia desc (0xe8 bytes) a obj+0x5e8  [desc+X → obj+0x5e8+X]

0x49d2f0 (método de G; G = [0x2982560], creado en 0x61bda):
  0x49d7fb  vmovups S+0x38..0x57 → G+0x4c..0x6b     (8 dwords)
  0x49d816  S+0x58 → G+0x74 ; S+0x5c → G+0x78
  0x49d826  S+0x64 → G+0x7c (selector FMV) ; S+0x65 → G+0x80 ; S+0x66 → G+0x84 ; S+0x67 → G+0x88
0x49e400  G+0x6c/70 ← G+0x4c/50 (8 bytes) ; luego 0x122c410(G, 1) → G+0x90 = 1 ("ultra") al iniciar
ctor 0x61b17/0x61b1c: G+0x4c..0x87 = 0xffffffff ; G+0x8c = 1, G+0x90 = 0
```

**Nombres de G (V, getter de variables del script de render `0x745b0`):**

| G | Variable de script | Notas |
|---|---|---|
| +0x4c/+0x50 | `width0` / `height0` | `halfwidth0` = /2, `quarterwidth0` = /4 (mismo campo, calculado) |
| +0x54/+0x58 | `offscreenwidth0` / `offscreenheight0` | |
| +0x5c/+0x60 | `lowres_width0` / `lowres_height0` | |
| +0x64/+0x68 | `lowres_offscreenwidth0` / `…height0` | |
| +0x7c | selector de `FMVx/y/w/h` | ≠0 → 0, 0, 1280, 720; =0 → 160, 90, 960, 540 (constantes) |

El getter del script devuelve los campos **sin mirar el modo**. Los consumidores que sí miran el modo son
dos getters (V):

```
0x63920 GetRenderSize(G,&w,&h):     G+0x90==1 → G+0x4c/50 · ==2 → G+0x5c/60 · ==0 → G+0x6c/70
0x63960 GetOffscreenSize(G,&w,&h):  G+0x90==1 → G+0x54/58 · ==2 → G+0x64/68 · ==0 → (nada)
0x63990 (G,&w,&h):                  G+0x74/78
```

`0x63920`/`0x63960` se llaman en `0x48d243/0x48d258` y `0x48e505/0x48e51a`, y el resultado va a
`0x1242b40(render_mgr, w, h)` y `0x1242b50(render_mgr, w, h)` (render_mgr = `[0x2afcbf0]`). `0x63920`
también se usa en `0x49e473`, que arma un vec4 `(G+0x6c, G+0x70, 1/G+0x6c, 1/G+0x70)` (I: constante de
tamaño de pantalla para shaders).

**Texturas de la cadena de swap (clase de `0x10d300`, V):**

| Objeto | Nombre (cadena) | Tamaño | Formato | Sitio |
|---|---|---|---|---|
| obj+0x6f8 | `RNA:MsaaTex` | desc+0x5c/60 | desc+0x58 | `0x110b44` |
| obj+0x708 → obj+0x760[0] | `RNA:SwapChainTex` ("High") | desc+0x5c/60 | desc+0x58 | `0x110ccb` |
| obj+0x710 → obj+0x760[1] | `RNA:SwapChainTex` ("Low") | desc+0x90/94 | desc+0x8c | `0x110e41` |
| obj+0x778 | `RNA:OverlaySwapChainTex` | desc+0x78/7c (solo si ambos > 0) | desc+0x74 | `0x110f3d` |
| obj+0x7c8 / 0x7d0 | `RNA:DefaultDST` / `RNA:OverlayDefaultDST` | derivados de SwapChainTex / Overlay | — | `0x111399`, `0x111526` |
| obj+0x6d0 → X | `RNA:VideoOutTex_High` / `_Low` × desc+0x6c | High = max(desc+0x5c, desc+0x78) × max(+0x60, +0x7c); Low = max(desc+0x90, +0x78) × max(+0x94, +0x7c) | `0x23`; VideoOut `A8R8G8B8Srgb` | `0x10dcdf..0x10dd7d` → `0x118b60` |

El `max` con desc+0x78 se aplica a High solo si existe el overlay (`cmpq $0,0x778(%rbx)` en `0x10dce7`);
a Low siempre (`0x10dd4c`, `0x10dd5e`). Los buffers de X se registran con `sceVideoOutSetBufferAttribute`
(`0x112247`, `pixelFormat 0x80000000`, aspect 0) y `sceVideoOutRegisterBuffers` (`0x112263`); los nombres
de las dos importaciones son I por la firma de argumentos (7 y 5 args, coinciden con el SDK).

El composite final (`0x117690`, cadena `OverlayPlane`) toma la escena de `obj+0x760[obj+0x770]`, el
overlay de `obj+0x778` y la salida de `0x118380(X)` = buffer actual de VideoOut (V).

---

## 3. Tabla campo → escritor → lectores → RT observado

Capturas: **R** = `frame_1792163000` (*Resolution*), **P** = `frame_1792163382` (*Performance*), **B** =
`frame_1791947138` (base). Todas con VideoOut 2160 salvo B (base, sin rama 1800).

| Campo (ancho) | Escritor dominante (bytes) | Lectores identificados | RT observado | Certeza |
|---|---|---|---|---|
| **S+0x38** | rama 1800: `0x533227 c7 00 80 0c 00 00` (3200) · normal: `0x533274 89 38` (edi ← imm `0x53325b`) | `0x49d7fb` → G+0x4c `width0`; mismo registro → desc+0x5c | R: escena B10G11R11/D32S8 `0x108e8c0000`/`0x1085980000` **3200×1800**; derivados 1600×1800, 160×1800 | Escritor V · lectores V · RT↔campo **I fuerte** (vía `0x63920` modo 1) |
| G+0x4c `width0` | `0x49d808` (copia de S) | `0x63920` modo 1 → `0x1242b40`; script `width0/halfwidth0/quarterwidth0`; `0x49e40e` → G+0x6c | R: escena 3200×1800 | V / RT I |
| G+0x6c `medium` | `0x49e41a` (= G+0x4c al iniciar) | `0x63920` modo 0; vec4 `0x49e478` | — (modo 0 no observado) | V |
| **S+0x40** | 1800: `0x533242 bf 40 06 00 00` (1600) · normal: edi | `0x49d7fb` → G+0x54 `offscreenwidth0` | R: linear depth R32F `0x1073a80000` **1600×900** y efectos 800×450; P: linear depth **1600×900** también | Escritor V · RT **I** (en P el par de modo 2 es 1920×1080, así que el 1600×900 de P sale de `offscreenwidth0` vía script, sin modo: **H**) |
| **S+0x48** | 1800: `0x533238 b9 80 07 00 00` (ecx=1920) · normal: edi | `0x49d7fb` → G+0x5c `lowres_width0`; mismo ecx → desc+0x90 | P: escena 1920×1080, derivados 960×1080, 96×1080 | Escritor V · RT I fuerte (`0x63920` modo 2) |
| **S+0x50** | igual que S+0x48 (`0x533298 89 4e 50`) | → G+0x64 `lowres_offscreenwidth0` → `0x63960` modo 2 | P: efectos/bloom **960×540** (= ½ de 1920×1080) | V / RT I |
| **S+0x58** | `0x5332a0 41 c7 46 58 80 07 00 00` (fijo 1920) | → G+0x74 → `0x63990` (1 llamada, `0x49e94a`, resultado sin uso visible) y `0x49ea53` → textura **`WebKitSurface`** (`G+0x80338`, fmt `0x61`) | Ninguno en las capturas de combate | V (lectores dentro de los métodos de G con el heurístico de §9) |
| S+0x64 | `0x5332b0` (= 1) | → G+0x7c → rect FMV `0,0,1280,720` | — | V |
| **desc+0x5c** | `0x5332c1 45 89 47 5c` (r8d = S+0x38) | `0x110b44` MsaaTex · `0x110ccb` SwapChainTex High · `0x10dcdf` VideoOutTex High (max) · `0x111274` buffer `w/8 × h/11` | R: tonemap RGBA8 `0x109ffe0000` **3200×1800** = escena del composite | V / RT **I fuerte** (tamaño + dirección contigua, §3.1) |
| **desc+0x78** | `0x5332d1 41 c7 47 78 80 07 00 00` (fijo 1920) | `0x110f3d` **OverlaySwapChainTex** · `0x10dcf1`/`0x10dd33` piso de VideoOutTex High/Low | R, P, B: UI/HUD RGBA8 `0x109eee0000` / `0x1080320000` **1920×1080**; depth UI `0x109c468000` 1920×1080 (OverlayDefaultDST, I) | V / RT **I fuerte** |
| **desc+0x90** | `0x5332e9 41 89 8f 90 00 00 00` (ecx = S+0x48) | `0x110e41` SwapChainTex Low · `0x10dd28` VideoOutTex Low (max con +0x78) | P: tonemap `0x109f760000` **1920×1080** (escena del composite `ddbe162b`) | V / RT I fuerte |
| VideoOutTex (presentado) | `0x118b60` con los `max` de arriba | `0x112160` registra el set actual; `0x118380` lo da al composite | R: `0x109a470000` 1920×1080 sRGB · P: `0x1098490000` 1920×1080 · B: `0x107ceb0000` 1920×1080 (log B: `RegisterBuffers width 1920`) | Escritor V · tamaño efectivo = `max(desc+0x90, desc+0x78)` **I fuerte**, ver §4.2 |
| Dispatch del composite | lo arma el guest | `ddbe162b` / `e5b29094` | R, P, B: **240×135×1** (= 1920×1080 / 8) | V captura · origen = tamaño de la salida, **I** |
| Buffer de máscara de `ddbe162b` | ? | slot 1 de `ddbe162b` | P: `0x109f750000`, **24576 B** (justo antes del SwapChainTex Low) | V captura · escritor/tamaño **no trazado** |

### 3.1 Por qué SwapChainTex/Overlay son esos RT (I fuerte)

- Las tres direcciones de P/R quedan contiguas y separadas `0x880000` (= 2048×1088×4, el tamaño alineado de
  1920×1080 RGBA8): UI `0x109eee0000`, Low `0x109f760000`, High `0x109ffe0000`. El código crea High (0x708),
  Low (0x710) y Overlay (0x778) en ese orden; el asignador baja de dirección (I).
- El tonemap escribe High en R y Low en P, y el composite muestrea exactamente ese RT más el de UI (V).
- En B (render 1600×900) la UI sigue en 1920×1080 y la salida también: la única fuente que vale 1920 en
  la rama base además de S+0x58 es desc+0x78 (la rama normal base pone S+0x38..0x54 y desc+0x5c/0x90 a
  1600) (V selector). S+0x58 queda descartado como UI porque su único lector es `WebKitSurface`.

---

## 4. Qué cambia la preferencia (*Favor Performance / Favor Resolution*)

### 4.1 Lo que sí está cerrado (V)

`0x428b90` (vfunc, solo si `0x63900` = 2, es decir Neo): modo `r15d` = 2 si `[LodManager+0x44] ≠ 0`; si
no, 2 cuando `R+0x92ec == 1` y 1 en otro caso. Después:

1. `0x122c410(G, modo)` → **G+0x90 = modo** (escribe solo si cambia; nombra el modo con la tabla
   `0x2505200`: 0 "medium", 1 "ultra", 2 "high").
2. G+0x8c = 1 o 2 (`0x428ca7`).
3. `0x112130(obj, modo == 2)` → **obj+0x770 = (modo == 2)** (escena final = SwapChainTex Low si 1, High
   si 0) y `0x112160(X, idx)` vuelve a registrar el set de VideoOut `X+0x18[idx]`.

Ningún escritor de la preferencia toca G+0x4c..0x78, S ni desc: **los anchos se fijan una vez al arrancar
por el selector y la preferencia solo elige pares** (V dentro del alcance de §9). Lectura de las capturas
coherente con eso: R = modo 1 (render G+0x4c = 3200×1800, offscreen G+0x54 = 1600×900, SwapChainTex High);
P = modo 2 (render G+0x5c = 1920×1080, offscreen G+0x64 → efectos 960×540, SwapChainTex Low) (I fuerte).
La asignación *Favor Performance* ↔ "high" (2) y *Favor Resolution* ↔ "ultra" (1) queda así
**explicada por tamaños**; qué setter escribe `R+0x92ec`/`LodManager+0x40` al cambiar el menú sigue sin
trazar (UFC-38).

### 4.2 Contradicción abierta: el set "High" de VideoOut

Por el código, modo 1 debería registrar `VideoOutTex_High` (3200×1800 en la rama 1800). Pero en R el
composite escribe y se presenta un buffer de **1920×1080** (dispatch 240×135), y la ventana de R no muestra
recorte (D, UFC-38). Lectura estática del bucle `0x118cc0..0x118da1`: los dos arrays (`X+0x18` y `X+0x20`)
parecen recibir el **mismo** objeto `r13`, construido primero como High y luego otra vez como Low con un
puntero de alias a `X+0x18[i]` (`0x118d4a`, `0x118d54..0x118d93`); el segundo objeto asignado en
`0x118d51` se descarta. Si es así, el set de VideoOut termina en 1920×1080 sea cual sea el índice, y eso
explicaría R. **H**: no lo doy por cerrado, el desensamblado lineal de ese bucle es raro y no hay log de
`RegisterBuffers` en try-1b. Lo resuelve una línea de log o un BP (§9, puntos L1/BP4).

Para la ruta elegida (§5) la contradicción **no importa**: en la rama normal High y Low valen lo mismo.

---

## 5. Qué hay que cambiar para 2560×1080 coherente, y qué ruta

### 5.1 Ruta elegida: Pro + VideoOut 1080 (rama normal) + *Favor Performance*

| Paso | Por qué |
|---|---|
| `BB_NEO=1` | Sin Neo el selector usa `edi = 1600` (`0x53325f`) y el `cmovne` no toma el imm de Pro |
| VideoOut con alto **≠ 2160** (`BB_INTERNAL_HEIGHT=1080`) | El selector solo compara el alto con `0x870` (`0x53320a`, V); así entra en la rama normal, donde **un solo registro** (`edi`) da el ancho de S+0x38/0x40/0x48/0x50 y de desc+0x5c/0x90. El ancho de `BB_INTERNAL_WIDTH` no lo lee nadie (UFC-40, I fuerte) |
| *Favor Performance* | En la rama normal los dos modos usan los mismos tamaños (pares iguales), así que el menú no cambia el contrato. Se elige *Performance* porque es el default (`R+0x92ec = 1`, D), es la condición del baseline de Test 1 y su captura P muestra justo la topología buscada: escena = UI = salida, composite `ddbe162b` 1:1. *Resolution* en rama 1800 usa `e5b29094` con escena normalizada ≠ salida, que con un 21:9 parcial deformaría (§5.3) |

Cambios mínimos de guest (diseño; **no** se creó XML):

| # | Sitio | Bytes originales | Valor nuevo | Qué arrastra | Necesario |
|---|---|---|---|---|---|
| W1 | imm de `0x53325a mov $0x780,%ecx` → **`0x53325b`** (archivo `0x53725b`) | `80 07 00 00` | `00 0a 00 00` (2560) | S+0x38/40/48/50 → G `width0`, `offscreenwidth0`, `lowres_width0`, `lowres_offscreenwidth0`; G+0x6c (init); desc+0x5c/0x90 → MsaaTex, SwapChainTex High/Low, VideoOutTex High/Low (`max(2560,1920)`) | **Sí** |
| W2 | imm de `0x5332d1 movl $0x780,0x78(%r15)` → **`0x5332d5`** (archivo `0x5372d5`) | `80 07 00 00` | `00 0a 00 00` | desc+0x78 → OverlaySwapChainTex (UI) y su depth; piso de VideoOutTex | **Sí** (si no, UI 1920 con salida 2560: `ddbe162b` lee la UI 1:1 y la franja derecha queda fuera de rango, UFC-40 §4.1) |
| W3 | imm de `0x5332a0 movl $0x780,0x58(%r14)` → `0x5332a4` (archivo `0x5372a4`) | `80 07 00 00` | `00 0a 00 00` | S+0x58 → G+0x74 → `WebKitSurface` | **No** para el primer ensayo (no hay consumidor de combate); dejarlo en 1920 reduce variables |

Las alturas (`0x533264 ba 84 03…` → 1080, `0x5332d9`, `0x5332a8`) no cambian.

### 5.2 Qué queda coherente con W1+W2 (I, por la tabla §3)

- Escena, depth, half/quarter, 1/20 (`2560/20 = 128`), offscreen y lowres: todos 2560×1080 (un registro).
- SwapChainTex High = Low = 2560×1080; Overlay (UI) = 2560×1080; VideoOutTex = `max(2560, 2560)`.
- Escena = salida → se espera **`ddbe162b`** (H: la elección del composite por relación escena/salida es
  I de UFC-38, no trazada) con dispatch 320×135.
- Host: el driver acepta `width ≤ pitch` y aspect 0 (V UFC-40); el presenter muestra 2.37:1 sin barras en
  una ventana 2560×1080 (V UFC-40 §6.1).

### 5.3 Alternativa descartada: VideoOut 2160 (rama 1800) + *Favor Performance*

Requeriría el imm `0x533239` (ecx: S+0x48/0x50 y desc+0x90) más W2. Funciona solo mientras el menú esté en
*Performance*: si el usuario pasa a *Resolution* el render vuelve al par S+0x38 = 3200×1800 (16:9) y
`e5b29094` lo comprime en una salida 2560×1080 (coordenada normalizada, UFC-40 §4.2) → imagen anamórfica.
Además deja el par High sin tocar. Por eso no es la ruta inicial.

### 5.4 Por qué "solo 3 inmediatos" no alcanza para un 21:9 correcto

Los inmediatos dan **tamaños coherentes**, no una imagen correcta:

1. **Aspect de cámara** (Claude 3): sin él la escena saldrá estirada en horizontal (H fuerte UFC-40 §3).
2. **Contenido del HUD**: el overlay medirá 2560, pero no sé si Scaleform dibuja en todo el ancho, se
   ancla a la izquierda o se estira; `StageScaleMode` y el viewport del stage no están trazados (H). Solo
   cierro el tamaño del RT.
3. **FMV**: el rect sale de constantes `0,0,1280,720` / `160,90,960,540` en un espacio virtual (V
   `0x7487f..0x748bb`); si el consumidor lo mapea a la salida completa, los videos 16:9 se estiran (H).
4. **Buffer de máscara de `ddbe162b`** (24576 B en P): no sé quién lo dimensiona. Si queda fijo para 1920,
   la máscara de UI de la franja 1920..2559 lee basura (H, riesgo principal del primer ensayo).
5. **Render script**: si algún pase usa tamaños absolutos (`setviewport` con números) en vez de las
   variables de G, quedará en 1920 (NM).
6. **Escritores no vistos**: el heurístico de §9 no prueba que nadie más escriba G+0x4c..0x78 (I).

---

## 6. Firmas candidatas (únicas en el eboot; no se creó ningún parche)

Búsqueda en `out/eboot.elf` completo (V, `bytes.find` sobre todo el archivo):

| Id | Firma (hex) | Coincidencias | Inicio (vaddr) | SignatureOffset del imm | Address para `scripts/patches.py` (base `0x400000`, según su docstring) |
|---|---|---|---|---|---|
| W1 | `45 85 e4 b9 80 07 00 00 bf 40 06 00 00 ba 84 03 00 00 0f 45 f9` | **1** | `0x533257` | +4 | `0x93325b` |
| W2 | `41 c7 47 78 80 07 00 00 41 c7 47 7c 38 04 00 00` | **1** | `0x5332d1` | +4 | `0x9332d5` |
| W3 | `41 c7 46 58 80 07 00 00 41 c7 46 5c 38 04 00 00` | **1** | `0x5332a0` | +4 | `0x9332a4` |
| (alt. 5.3) | `b9 80 07 00 00 bb 84 03 00 00 bf 40 06 00 00` | **1** | `0x533238` | +1 | `0x933239` |

Ojo: `b9 80 07 00 00` solo aparece **2 veces** (`0x533238`, `0x53325a`); no usar firmas cortas. `run.sh`
ya pasa `--title-id CUSA06534 --app-version 01.14` y `BB_PATCHES_DIR` a `patches.py` (V), pero el
docstring y `TITLE_ID` por defecto de `patches.py` siguen siendo los de CTR: verificar la base de
direcciones con un parche de prueba nulo antes de confiar en `Address`; con `Signature` +
`SignatureOffset` el sitio queda fijado aunque la base difiera.

Licencia: firmas propias, sin bytes de terceros; el sitio de mancelli (`0x533260`) no se usa.

---

## 7. Dependencias y rollback conceptual

- **Orden**: W1 y W2 son de arranque (el selector corre una vez al abrir VideoOut); no hay estado que
  migrar. No tocan guardados ni la opción del menú (los anchos no se guardan; la opción RESOLUTION sí
  persiste en `user/`, D UFC-38: anotar su valor).
- **Rollback**: quitar el XML → `Patches: 0 writes` en el log. No hay nada más que deshacer.
- **Depende de**: `BB_NEO=1` y VideoOut alto 1080. Con VideoOut 2160 W1 no tiene efecto sobre la rama 1800
  (sus imm son otros) y el resultado sería 16:9 en la escena con W2 dando UI 2560 (incoherente): **no
  combinar W2 con VO 2160**.
- **Independiente de**: el parche de aspect de cámara (otro ticket) y del host. Activarlos por separado.
- **Online**: no habilitar con W1/W2 hasta tener evidencia offline (UFC-40 §7).

---

## 8. Criterio de una futura prueba T21-1 (no ejecutada, no pedida aquí)

Config: igual que Test 1 (Neo 1, `BB_INTERNAL_WIDTH/HEIGHT` con alto 1080, límite 60, 0 parches salvo
W1+W2), ventana `BB_WINDOW_SIZE=2560x1080`, *Favor Performance*, `BB_CAPTURE_ON_FLIP=1` + `capture.trigger`
en el ring, y nivel de log que incluya `Lib.VideoOut <Info>` (L1). Sin benchmark.

**Pasa (contrato coherente)** si la captura muestra a la vez:
- escena B10G11R11 y depth D32S8 **2560×1080**; derivados 1280×1080, 128×1080, efectos 1280×540;
- tonemap (SwapChainTex Low) **2560×1080**; UI RGBA8 y su depth **2560×1080**;
- composite `ddbe162b` con **groups 320×135×1**, escribiendo el buffer presentado sRGB **2560×1080**;
- log: `Patches: 2 writes`, `RegisterBuffers … width = 2560, height = 1080, pitchInPixel ≥ 2560`;
- sin `Guest fault`/STOP en el combate.

Se espera (y **no** es fallo de este ensayo) imagen estirada por la cámara 16:9.

**Diagnóstico si falla:**
| Síntoma | Lectura |
|---|---|
| UI en 1920 con salida 2560 | El overlay no sale solo de desc+0x78 (otra fuente fija) → BP2 |
| Dispatch 240×135 o salida 1920 | La salida no sale de `max(desc+0x90, desc+0x78)` → L1/BP3/BP4 |
| Franja derecha con UI rota y todo en 2560 | Máscara de `ddbe162b` dimensionada aparte → BP6 |
| Algún pase de escena en 1920 | Tamaño absoluto en el script o RT fijo → buscar ese pase en la captura |
| Composite `e5b29094` con escena = salida | La elección del composite no es por relación escena/salida → revisar UFC-38 §1.2 |

---

## 9. Lo que no pude cerrar estáticamente: logs y breakpoints futuros

Alcance estático real: desensamblado lineal de `0x1000..0x1e00000`; escritores/lectores de G buscados en
funciones que usan los offsets propios de G (`0x802f8…0x80398`) y por patrón; lectores de desc buscados
por los offsets de la copia `obj+0x644..0x67c`. **No es exhaustivo** para accesos indirectos.

Primero lo barato y sin GDB:

| Id | Qué | Resuelve |
|---|---|---|
| **L1** | Nivel de log con `Lib.VideoOut <Info>` en la próxima corrida que se haga de todos modos (líneas `SetBufferAttribute`/`RegisterBuffers` ya existen, `video_out.cpp:28`, `driver.cpp:217`) y cambiar el menú en caliente | §4.2: si al pasar a *Resolution* se registra 3200×1800 o 1920×1080 |

GDB (requiere **autorización explícita** cuando se proponga ejecutarlo; no en este ticket). Runtime =
`0x800000000 + vaddr`:

| Id | BP (runtime) | Registrar | Resuelve |
|---|---|---|---|
| BP1 | `0x80049d7fb` | `r14` (S), `[r14+0x38..0x67]`, `r15` (G) | Valores reales de S al copiarlos a G (rama tomada, imm parcheados) |
| BP2 | `0x800110f3d`, `0x800110ccb`, `0x800110e41` | `[r14+0x660/664]`, `[r14+0x644/648]`, `[r14+0x678/67c]` | Tamaños con que se crean Overlay y SwapChainTex High/Low |
| BP3 | `0x80010dd7d` (call `0x118b60`) | `ecx, r8d, r9d, [rsp], [rsp+8]` | Tamaños High/Low y nº de buffers de VideoOutTex |
| BP4 | `0x800112247` y `0x800112263` | `r8d/r9d` y `[rsp]` (ancho/alto/pitch), `esi`/`[r12+0x48]` (índice), retorno | Qué set se registra en cada cambio de modo (§4.2) |
| BP5 | `0x800428c9d` (call `0x122c410`) | `esi` (modo), `r13d`, `[rbx+0x92ec]` | Modo que produce cada opción del menú |
| BP6 | watch de escritura sobre el buffer slot 1 de `ddbe162b` (dirección de la captura de esa corrida) + `bt` | quién lo crea y con qué tamaño | Riesgo 4 de §5.4 |
| BP7 | `watch -l *(long*)(G+0x4c)` … `G+0x78` (G = `*(long*)0x802982560`) desde el arranque | escritor y `bt` | Que no haya otros escritores de G además de `0x49d808`, `0x49e41a` y el ctor |
| BP8 | `rwatch` sobre `obj+0x660` (copia de desc+0x78; obj = `S+0x20`) | lectores y `bt` | Lectores del ancho de overlay fuera de la clase `0x10d300` (p. ej. Scaleform) |

---

## 10. Correcciones a UFC-40 / UFC-38

- UFC-40 §2.1: "bastaría cambiar 3 inmediatos (`0x53325b`, `0x5332a4`, `0x5332d5`)". **Corregido**:
  `0x5332a4` (S+0x58) solo alimenta `WebKitSurface`; los obligatorios son `0x53325b` y `0x5332d5`, y solo
  en la rama normal (VO 1080).
- UFC-40 §1.1 / UFC-38 §2: "`S+0x48..0x5c`, `desc+0x78/7c`, `desc+0x90/94` = UI/salida". **Refinado**:
  S+0x48/0x50 = par `lowres` de G (render y offscreen del modo "high"); desc+0x90 = SwapChainTex Low y
  ancho de VideoOut; desc+0x78 = overlay/UI y piso de VideoOut; S+0x58 = WebKit.
- UFC-40 §1.1: "`S+0x40/44` = linear depth". **Refinado**: es `offscreenwidth0`; el linear depth 1600×900
  aparece igual en P, donde el offscreen del modo es 1920×1080 (H: el pase usa la variable sin modo).
- UFC-36 (D) dejaba sin escritor G+0x4c..0x70: **resuelto** (`0x49d7fb`, `0x49e40e`, ctor `0x61b17`).

---

## 11. Pendiente

- §4.2 (set High de VideoOut): L1 o BP4.
- Tamaño del buffer de máscara de `ddbe162b` (BP6) y contenido del HUD en un overlay de 2560 (Scaleform).
- Aspect de cámara: ticket de Claude 3; T21-1 solo valida tamaños.
- Rect FMV en espacio 1280×720 fijo: comportamiento en 21:9 sin evaluar.
- Ninguna prueba hecha; nada medido.
