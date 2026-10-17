# UFC-51 — Release Windows 2e700eb: pelea de Career a ~30 FPS (antes ~68)

## Segunda pasada (TESTexe de 2e700eb con windows, PREUPLOAD=1): 47 contra 67 FPS

Logs nuevos (misma PC):
- `release-2e700eb-win-TESTexe-20261016_220152.log`: `UFC3-WRITE-WINDOWS-TEST.exe` de 2e700eb,
  STEP=1, WINDOW=1, DIAG=1, PREUPLOAD=1, caché de shaders **no** borrada.
- `release-2e700eb-win-windows-on-20261016_215105.log`: `UFC3.exe`, el `set` no llegó (sin
  windows), PREUPLOAD=1, caché no borrada.

Ventanas de pelea: las que tienen ≥12 host copies/frame y 550–1000 draws (la firma de la pelea
en los cuatro logs). Promedios:

| pelea de Career | test5 (de4e15e) | TESTexe (2e700eb) | windows-on (sin windows) | release (sin windows) |
|---|---|---|---|---|
| ventanas de 5 s | 38 | 76 | 10 | 24 |
| PREUPLOAD / windows | 0 / sí | 1 / sí | 1 / no | 1 / no |
| FPS (ms/frame) | 66.7 (15.0) | 46.9 (21.3) | 21.6 (46.4) | 28.9 (34.5) |
| draws/frame, us/draw | 740, 15.5 | 775, 16.2 | 717, 16.0 | 683, 14.8 |
| hilo GPU, ms/frame | 11.4 | 12.5 | 11.5 | 10.1 |
| draw pipe: stage B ocupada / stage A esperó | 78 % / 48 % | 58 % / 36 % | 19 % / 10 % | 23 % / 11 % |
| guest esperó al frame previo, ms/frame | **4.3** | **1.2** | 2.0 | 1.0 |
| drains por `commands` (readbacks al hilo GPU): /s, ms/frame | 60, **2.8** | 74, **6.0** | 135, 3.5 | 150, 2.5 |
| drains por `rasterizer`: /s, ms/frame | 67, 4.4 | 47, 1.7 | 24, 1.1 | 30, 1.2 |
| hilo GPU bloqueado en GPU ticks: %, ms/frame | 15.9, **2.4** | 20.1, **4.3** | 23.6, 10.9 | 24.3, 8.4 |
| host copies: % del hilo GPU (por frame) | 7.2 % (12.1) | 5.3 % (15.6) | 2.6 % (17.6) | 2.9 % (15.0) |
| write faults/frame | 29 | 37 | 43 | 38 |
| readbacks/s (por frame) | 86 (1.3) | 77 (1.65) | 301 (14) | 310 (10.7) |
| stepped writes/s, windows/s | 711, 430 | 678, 349 | — | — |
| pre-upload en la pelea (por 600 frames) | — | 66 MB al entrar, después 1–5 MB | | |

### 1. Qué explica 47 contra 67

- **No es el hilo GPU.** Hace 12.5 ms de trabajo por frame contra 11.4 (+1.1 ms) en la misma
  escena (775 contra 740 draws). En test5 el hilo GPU es el cuello de botella: stage B al 78 % y el
  juego espera 4.3 ms por frame al frame anterior. En TESTexe ya no lo es: stage B al 58 % y el
  juego espera 1.2 ms. El frame se pierde **antes** de la GPU, en la serialización con el juego.
- **Lo que crece (por frame, TESTexe − test5):**
  - drains por readbacks (`commands`): **+3.2 ms** (6.0 contra 2.8). Son 74/s contra 60/s, y
    cada uno cuesta más: 3.8 ms contra 3.1 ms.
  - esperas del hilo GPU a ticks de la GPU: **+1.9 ms** (4.3 contra 2.4).
  - trabajo del hilo GPU: **+1.1 ms**.
  - Los drains por `rasterizer` bajan 2.7 ms. Los tramos se solapan, así que no se suman exacto,
    pero los readbacks son la mitad de los 6.3 ms.
- **La mezcla de readbacks no es la misma.** El total por segundo es parecido (77 contra 86). En
  la pelea de test5 todos son `+0x4219b8 (from +0x409dfe) [page > max]`, ~66/s. En TESTexe hay
  ~48/s de memcpy `+0x431c4f [near GPU, 3824 GPU B]` y ~24/s de `+0x4219b8 [page > max]`. Ese
  memcpy no aparece en la pelea de test5. Fuera de la pelea test5 lo tiene vetado por UFC-48
  (`[host copy, 0 GPU B]`, readback igual). En TESTexe escribe a ≤16 bytes de datos GPU
  (`near`): ninguna window ni step lo puede tomar y cada vez hay drain + espera a la GPU. Desde el
  código, que esa página tenga bytes GPU no depende de PREUPLOAD: `gpu_modified_ranges` sólo crece
  con bindings escritos por la GPU (`buffer_cache.cpp`, `ObtainBuffer`). Lo más probable es que
  dependa de la pelea concreta y del layout del heap de esa ejecución. No está demostrado.
- **PREUPLOAD, con números:**
  - Lo que sube de fondo en la pelea: 66 MB al entrar y después 1–5 MB cada 600 frames
    (~0.3 MB/s). Ese ancho de banda no cuenta.
  - Efecto indirecto posible: re-proteger páginas al sincronizarlas. Hay +8 write faults/frame
    (37 contra 29), a decenas de µs cada uno: ≤0.3 ms/frame.
  - El otro efecto de PREUPLOAD: las texturas salen de la arena cuando está residente
    (`ObtainBufferForImage`: `SynchronizeMemory` en el hilo GPU + la barrera de lectura sobre la
    arena en `tile_manager.cpp`). Eso es compatible con el +1.1 ms de hilo GPU y el +1.9 ms en
    ticks, pero ningún contador lo aísla.
  - Del lado de PREUPLOAD hay como mucho ~3 ms/frame. Los otros ~3 ms vienen de readbacks que
    PREUPLOAD no genera.
- **Host copies y us/draw no explican la diferencia.** El 5.1 % de host copies del ticket es
  tiempo del hilo GPU esperando copias. En la pelea son 5.3 % de 21.3 ms = 1.1 ms/frame, igual que
  test5 (7.2 % de 15.0 ms = 1.1 ms). El ~2.7 % es de la escena previa o del release sin windows,
  no de la pelea de test5. Los 17.56 us/draw son una ventana con 699 draws. El promedio de la
  pelea es 16.2 contra 15.5. Los 10–15 us/draw corresponden a la escena previa (~640 draws).
- **Las dos ejecuciones difieren en más que PREUPLOAD:**
  - el launcher de 2e700eb (UFC-28) escribe `readbacks.csv` y `frames.csv` junto al log cuando
    las estadísticas están activadas; el de de4e15e no.
  - el log va a `Z:\logsUFC` (test5: `C:\…\AppData\…\logs`). `readbacks.csv` se escribía con un
    `fflush` por readback en el hilo de comandos GPU, **dentro del drain**: si Z: es una unidad de
    red, cada readback lleva una escritura de red adentro. Eso es compatible con los +0.7 ms por
    drain.
  - caché de shaders llena contra borrada.
  - `BB_WINDOW_SIZE=1920x1080` contra sin definir.
  - otra pelea, con otro heap.
- **Prueba de que esas diferencias pesan:** windows-on y release usan el mismo exe y los dos
  tienen PREUPLOAD=1 y no tienen windows. Aun así, la pelea va a 21.6 contra 28.9 FPS (46 contra
  35 ms) con readbacks/s iguales (~300). Entre esas dos ejecuciones cambian sólo la caché, el
  `BB_WINDOW_SIZE` y la pelea. Hay ±30 % entre ejecuciones sin tocar PREUPLOAD.

**Conclusión 1:**
- Los 6.3 ms/frame se reparten así: ~3 ms de readbacks más caros y algo más numerosos (memcpy
  `+0x431c4f` near-GPU), ~2 ms de esperas a la GPU y ~1 ms de hilo GPU.
- Los logs **no** permiten atribuirlo a PREUPLOAD. Su subida de fondo es despreciable. Su camino
  de texturas puede explicar como mucho la mitad, y nada lo aísla.
- Confianza en que "PREUPLOAD es la causa": baja-media. Hace falta la prueba de abajo.

### El log windows-on (70–80 FPS con 600–700 draws): no contradice la tabla

- Los tramos de 70–80 FPS (ventanas 51–75 y 97–104) **no son la pelea**. Tienen la firma de la
  escena previa: 2–4 host copies/frame (25 "not needed"), ~130 000 texture-set misses por
  ventana y readbacks ~110/s de `+0x1242a67`/`+0x47dada` `[other]`. No aparece ningún sitio de
  la pelea (`+0x4219b8`, `+0x431c4f`, `+0x3f31cc`).
- Es la misma escena que en la tabla de la primera pasada aparece como "escena previa ~640 draws,
  70 FPS". También aparece en TESTexe (ventanas 13–30 y 49–68). Durante ella el juego guarda
  `CAREERSAVE2` y carga Lua de `career/helper/contractfighthistoryhelper` y `main/vvms/flowdata`:
  es el hub/menús de Career con el luchador en 3D.
- La pelea de ese log son las ventanas 84–91: **21 FPS**, 301 readbacks/s, 14 por frame. Confirma
  la primera pasada (sin windows, la pelea va a 20–30 FPS).

### 2. ¿Sangre sin el costo?

PREUPLOAD=1 hace dos cosas (`buffer_cache.cpp`):
- (a) la subida de fondo (`Preupload`, ≤16 MB/frame);
- (b) en `ObtainBufferForImage`, las texturas salen de la arena cuando está residente
  (`SynchronizeMemory`), en vez de una copia staging desde la RAM del juego.

En la pelea, (a) sube 1–5 MB por 600 frames: no cuesta nada medible. Si PREUPLOAD tiene costo,
está en (b).

UFC-49 no identificó el mecanismo de la sangre. La hipótesis 3 de ese doc ("RAM obsoleta frente a
arena vigente") apunta a (b): una textura cuyo dato vigente sólo está en la arena, porque el
tracking perdió la marca GPU, sale bien de la arena y vieja de la RAM. En ese caso la sangre
necesita (b), y limitar (a) a "lo que la sangre necesita" no ahorra nada porque (a) ya es casi
cero.

El arreglo real de la sangre sería encontrar la textura/recurso y la marca que se pierde, para
que (b) deje de ser necesario. Requiere capturar el recurso (fuera de este ticket).

Si la prueba de abajo muestra que PREUPLOAD cuesta, el paso siguiente es un modo
`BB_PREUPLOAD=3` = sólo (b) sin (a), o sólo (a) sin (b). Así se ve qué mitad da la sangre y qué
mitad cuesta. Antes de esa prueba sería especular, así que no lo implementé.

### 3. Cambio hecho y una prueba que discrimina

**Hecho (acotado, no es el arreglo de los FPS):** `readbacks.csv` (`BB_READBACK_LOG`, el que
activa el launcher con las estadísticas) ya no hace `fflush` por línea. Ahora vacía el buffer
como mucho una vez por segundo. Esa escritura corre en el hilo de comandos GPU mientras el draw
pipe está drenado por el readback. Quita la escritura de red de cada drain en las sesiones de
diagnóstico con el log en `Z:`. No cambia nada si las estadísticas están desactivadas (el uso
normal).

**Arreglo de los FPS:** ninguno todavía. No hay una causa única demostrada.

**Compilación:** Linux `ufc3-build` exit 0 (`ufc3-v0.1.4-66-g5082e57`). No hice el build de
Windows ni el zip: el entorno del agente no tuvo permiso para tomar el lock de compilación
compartido. La prueba de abajo no los necesita.

**La prueba (una sola, con el zip de 2e700eb que ya está en esa PC, sin compilar nada):**

```
set BB_PREUPLOAD=0
UFC3-WRITE-WINDOWS-TEST.exe
```

Todo lo demás igual que en `220152`: misma carpeta de log (`Z:`), estadísticas activadas, mismo
ajuste de pantalla y caché sin borrar. Misma pelea larga de Career. El launcher respeta
`BB_PREUPLOAD` del entorno (`managed_env`/`game_env`). El log debe dejar de mostrar líneas
`Pre-upload:`.

Cómo leer el resultado:
- **La pelea vuelve a ≥60 FPS:** PREUPLOAD cuesta ~6 ms/frame en Windows. Siguiente paso: el
  split (a)/(b) de arriba para quedarse con la mitad que da la sangre. La sangre debería faltar
  en esta prueba; eso confirma UFC-49 en Windows.
- **Sigue en ~47 FPS:** PREUPLOAD no es la causa. La diferencia con test5 está en la ejecución:
  los readbacks near-GPU de `+0x431c4f` (mirar `Readback sites`), el log/CSV en `Z:` o la caché.
  Siguiente paso: el mismo test con la carpeta de log en `C:` (o un zip de esta rama, con el CSV
  vaciado una vez por segundo).
- En los dos casos, anotar en `Readback sites` de la pelea si sigue `+0x431c4f [near GPU]`. Si
  aparece con ~48/s y la pelea sigue lenta, el siguiente sospechoso es ese sitio.

## Primera pasada


Estado: análisis de logs y código, sin cambios de código. Causa identificada con certeza alta.
Falta la prueba de confirmación del usuario (abajo).

Logs (misma PC: RTX 3060 Laptop, 480 Hz, caché de shaders borrada en los dos):
- `release-2e700eb-win-20261016_213336.log`: `UFC3.exe`, PREUPLOAD=1, STEP=0, WINDOW=0.
- `UFC48-test5-win-20261016_210717.log`: `UFC3-WRITE-WINDOWS-TEST.exe` (de4e15e), PREUPLOAD=0,
  STEP=1, WINDOW=1.
- Los logs 0.1.4 de `users-20261016/` no tienen `Frame stats` (y son AMD): no sirven para comparar FPS.

## Números (promedio de ventanas de 5 s)

| | release, pelea (26 vent.) | release, escena previa ~640 draws (13) | test5, pelea Career (27) |
|---|---|---|---|
| FPS / mediana de frame | 29.1 / 34.7 ms | 70.0 / 13.9 ms | 67.4 / 15.4 ms |
| draws/frame, us/draw | 670, 15.2 | 638, 12.1 | 719, 15.7 |
| hilo GPU por frame (us/draw × draws) | **10.0 ms** | 7.8 ms | **11.2 ms** |
| Draw pipe: draws/s, stage B ocupada | 30 600, 23 % | 52 300, 49 % | 74 600, 80 % |
| write faults/s (por frame) | 1084 (37) | 1066 (15) | 1923 (29) |
| **readbacks/s (por frame)** | **312 (10.7)** | 104 (1.5) | **68 (1.0)** |
| Stepped writes / write windows por s | — / — | — / — | 877 / 501 |
| guest esperó al frame previo | 0.9 ms | 3.1 ms | 4.4 ms |
| Replay Job sema, ms/frame | 28 | 13 | 12 |

La post-pelea (release, 2 ventanas, 1166 draws, 2.0 readbacks/frame) vuelve a 54 FPS con *más*
draws: el FPS sigue a los readbacks, no a la cantidad de draws.

## Respuestas

1. **No es costo del hilo GPU.** El hilo GPU trabaja ~10 ms por frame en la pelea del release y
   ~11 ms en la de test5 (que va a 67 FPS); los 15 us/draw son de la escena de pelea (test5 tiene
   15.7). El draw pipe está parado (stage B 23 % contra 80 %) y el juego casi no espera a la GPU.
   Lo que cambia son los **readbacks: 10.7 por frame contra 1.0**. Cada readback es drain del draw
   pipe + esperar a que la GPU termine + bajar la página (GROK_HANDOFF §16): serializa CPU y GPU.
   (34.7 − 15.4 ms) / (10.7 − 1.0) ≈ 2 ms por readback. Sitios en la pelea del release
   (`Readback sites`, por ventana): memcpy del host en 0x100076de20 (~405), `+0x409d0f` (~365),
   memcpy en 0x1038d157c0 (~300), `+0x3f3562` (~143), `+0x3ff5c2`/`+0x3ff254` (~135),
   `+0x12590ae` (~125), `+0x47d855` (~75): los mismos que las write windows quitaron en
   GROK_HANDOFF §17. Pre-upload, compilaciones y texturas no lo explican: pre-upload movió
   0–3.7 MB cada 600 frames; casi todas las ventanas de 30 FPS tienen 0 compilaciones; los
   `texture set misses: generation` son del mismo orden en test5 (80 447 contra ~99 000).
2. **Las ventanas de 30 FPS son la pelea.** Coinciden con la pelea de test5 (≈700 draws, ≈15
   us/draw) y aparecen los sitios de readback propios de la pelea (`+0x3f3562`, ya visto en el
   attract fight). "Replay Job 0-3" son hilos del pool de jobs del juego esperando trabajo en el
   mismo semáforo que "Sim Job" (`+0xddffec<+0x1f14398`). Aparecen también en menús (3.9 ms) y en
   test5 (12 ms). Esos ms crecen con la duración del frame: son síntoma, no causa. No es una
   repetición.
3. **La variable es la falta de write windows (con STEP=0), no PREUPLOAD ni la caché.**
   - Con `BB_WRITE_STEP=0`, `DecideWriteStep` sale antes de decidir nada (`buffer_cache.cpp`,
     `!WriteStepEnabled()` → readback). Toda escritura CPU sobre una página GPU-modificada termina
     en `ReadMemory`. Por eso los sitios dicen `[other, 0 GPU B]`: el motivo ni se calcula. Ese
     mismo gate deja las windows inertes con STEP=0 aunque WINDOW=1 (UFC-2,
     `"inactive: BB_WRITE_STEP=0 opens none"`).
   - PREUPLOAD no entra en ese camino. Sólo sube páginas CPU-modificadas *sin* modificación GPU y
     cambia de dónde salen las texturas. No crea readbacks.
   - Caché fría: test5 también la borró y va a 67 FPS. Además, el FPS se queda en 30 durante
     20+ ventanas sin compilar nada.

Contexto: el launcher de Windows tiene `BB_WRITE_WINDOW=0` desde el hotfix 0.1.4 (386be2d,
corrupción del heap) y `BB_WRITE_STEP=0` desde UFC-49 (por la piel). Entonces 0.1.4 en Windows ya
debería tener este mismo ~30 en peleas de Career en esa PC. El "antes ~68" viene del exe de prueba
con windows, no de un release anterior. test5 (con el arreglo de coherencia de UFC-48, de4e15e,
que ya está en 2e700eb) jugó Career + Quick Fight sin `GPU wait stuck` ni `Device lost` y salió
con código 0.

**Linux también:** `run.sh` exporta `BB_WRITE_WINDOW=1 BB_WRITE_STEP=0` (4b644a8). Con STEP=0 las
windows no abren, así que Linux debería tener la misma pérdida en peleas (las 140–190 FPS de §17
eran con STEP=1).

## Arreglo mínimo propuesto (no implementado)

- **Opción A (config, trivial):** en el launcher de Windows (y en `run.sh`), `BB_WRITE_WINDOW=1`
  y `BB_WRITE_STEP=1`: es justo la config de test5. Riesgos que decide el Jefe: (a) el crash de
  heap que motivó el hotfix 0.1.4 (test5 no lo reprodujo con de4e15e, pero es una sola sesión);
  (b) la piel de UFC-49, que se atribuyó a STEP sin aislarlo. Hay que mirar la piel creada/career
  en la misma prueba.
- **Opción B (código, no trivial):** separar las windows del gate de STEP en `DecideWriteStep`.
  Con STEP=0, las faltas que irían a step o a window abrirían window, nunca un single-step.
  También hay que ajustar el snapshot de `ReadMemory` (`WriteStepEnabled() &&
  WriteWindowsEnabled()`) y el aviso "inactive" de `page_manager.cpp`. Permite WINDOW=1 sin
  tocar STEP, pero cambia la semántica que validó UFC-48: hay que medirlo y auditarlo antes.

## Una prueba (Windows, misma PC, misma pelea de Career)

```
set BB_WRITE_WINDOW=1
set BB_WRITE_STEP=1
UFC3.exe
```

PREUPLOAD queda en 1 (el default del release) y la caché se borra igual que antes. El launcher
respeta las variables del entorno. Predicción: la pelea pasa de ~29 a ~60–70 FPS. En el log, los
`readbacks/s` deberían bajar de ~310 a ~70 y aparecer `Stepped writes`/`Write windows` en cientos
por segundo. Si mejora, quedan descartados PREUPLOAD (sigue en 1) y la caché como causa, y se
confirma la opción A. Mirar también la piel del luchador creado y si hay crash al cargar la
segunda pelea o al firmar contrato. Si sigue en ~30 con los readbacks abajo, la hipótesis cae.
