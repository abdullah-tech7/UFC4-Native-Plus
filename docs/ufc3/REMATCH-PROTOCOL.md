# UFC 3 online: contrato de revancha (UFC-27)

Investigación estática, 16 oct 2026. Base `ufc3-online` 8de5098. Solo informe: no se ejecutó el juego,
no se tocó el servidor ni el runtime y no hubo build.

Fuentes:

- `out/guest-live.bin` (volcado vivo, hace falta para la reflexión TDF) y `out/guest-image.bin`.
  Las direcciones son offsets de la imagen guest; en ejecución, guest = 0x800000000 + offset. El
  código es igual en los dos archivos.
- `out/ufc3-tdf-schema.txt`, el esquema que extrae `tools/ufc3_tdf_schema.py`.
- `logs/server-20261015_234056.log`.
- Lectura (sin cambios) de `tools/ufc3_games.py` y `tools/ufc3_server.py`.
- Referencias: `~/Projects/refs/PocketRelay` (41a2270) y `~/Projects/refs/BF4BlazeEmulator`.

## 0. Resumen para el Jefe

1. **`0x13` es `replayGame`.** Sale del switch de nombres de comandos GameManager del propio eboot
   (no es una suposición por versión del SDK). El pedido es `ReplayGameRequest { GID uint64 }` y la
   respuesta **no lleva cuerpo**.
2. **Lo pide solo el host** (`Player_1`, 192.168.1.2) y solo cuando su `Game` local está en
   `POST_GAME (4)`. El guest no manda nada por Blaze para la revancha.
3. **La negociación "Rematch sí/no" es P2P, no Blaze.** Usa dos mensajes NetGameLink, `0x15` (guest→host)
   y `0x14` (host→guest). Blaze solo interviene después, cuando el host llama `replayGame`.
4. **Lo que el cliente necesita del servidor:** una respuesta vacía con éxito y
   `NotifyGameStateChange (0x64) {GID, GSTA=PRE_GAME 0x82}` para **los dos** jugadores. Sin esa
   notificación, el `Game` local sigue en `POST_GAME`. El siguiente `startGame` del host
   (`0xf22440`) exige `PRE_GAME` y falla en local sin mandar nada al servidor.
5. **No corresponde `NotifyGameReset (0x70)` ni `resetDedicatedServer (0x19)`.** Son de servidores
   dedicados: `0x70` lleva la sala completa (`DATA`) y la topología acá es P2P full mesh (`NTOP 130`).
   El cliente UFC **no tiene ningún comando `resetGame`**.
6. **Hoy:** el servidor contesta `ERR_COMMAND_NOT_FOUND` y el código UFC lo convierte en
   resultado de fallo `6` para la operación de revancha pendiente. Por eso "no pasa nada".

## 1. Evidencia del log

`logs/server-20261015_234056.log`, juego 2 (Player_1 es el host, myzkW el guest):

| Hora | Línea | Evento |
|---|---|---|
| 00:05:10.277 | 1022 | `Player_1 (host, …192.168.1.2) vs myzkW (…192.168.1.129) in new game 2` |
| 00:05:10.280 | 1029–1032 | host `advanceGameState GSTA=130` → `state -> 0x82` |
| 00:05:28.713 | 1208–1211 | host `advanceGameState GSTA=131` → `0x83` (la pelea arranca) |
| 00:08:04.682/.693 | 1392, 1406 | los dos mandan `GameReporting (0x1c) cmd 1` con `GRID 2` (sin handler: respuesta vacía) |
| 00:08:04.692 | 1400–1405 | host (msg 122) `advanceGameState GID 2 GSTA 4` → `0x4`, dos `NotifyGameStateChange` |
| 00:08:27.092 | 1440 | host (msg 126, misma secuencia que 122/124/125) `comp 4 cmd 0x13`, 5 bytes, `GID 2` |
| 00:08:27.093 | 1442 | `rejected command 0x13 for Player_1: unsupported GameManager command` |
| después | 1443– | solo telemetría `0xab` de los dos; ningún otro pedido GameManager |

Los 23 s entre `POST_GAME` y `0x13` coinciden con la pantalla de fin de pelea más la votación P2P
de revancha (§3). El guest (msg 13x) no manda ningún pedido de revancha a Blaze.

## 2. Qué es `0x13` en UFC (Blaze del eboot)

### 2.1 Nombres de comandos y notificaciones

Se decodificaron las dos tablas de saltos sin asumir IDs de ME3/BF4:

- Comandos: función `0xf09b00`, tabla `0x1f2ae38`, índice = ID − 1.
- Notificaciones: función `0xf0a890`, tabla `0x1f2b160`, índice = ID − 3.

Lo relevante:

| ID | Comando (UFC) |
|---|---|
| `0x03` | advanceGameState |
| `0x13` | **replayGame** |
| `0x14` | returnDedicatedServerToPool |
| `0x19` | resetDedicatedServer |

No existe ningún comando `resetGame`. Las entradas `0x0c`, `0x15` y `0x1c`/`0x1d` están vacías.

| ID | Notificación (UFC) |
|---|---|
| `0x64` | NotifyGameStateChange |
| `0x70` | NotifyGameReset |
| `0x71` | NotifyGameReportingIdChange |

### 2.2 Tipos (reflexión UFC, `out/ufc3-tdf-schema.txt`)

```
Blaze::GameManager::ReplayGameRequest            (líneas 2069-2070)
    GID  gameId                 uint64_t
Blaze::GameManager::NotifyGameStateChange        (1879-1881)
    GID  gameId                 uint64_t
    GSTA newGameState           Blaze::GameManager::GameState
Blaze::GameManager::NotifyGameReportingIdChange  (1861-1863)
    GID  gameId                 uint64_t
    GRID gameReportingId        uint64_t
Blaze::GameManager::NotifyGameReset              (1864-1865)
    DATA gameData               Blaze::GameManager::ReplicatedGameData
```

`GameState` de UFC (tabla de reflexión del enum en `0x2ab5130..0x2ab52a0`, `guest-live.bin`):

| Valor | Estado |
|---|---|
| 0 | NEW_STATE |
| 1 | INITIALIZING |
| 2 | INACTIVE_VIRTUAL |
| 3 | CONNECTION_VERIFICATION |
| 0x82 | PRE_GAME |
| 0x83 | IN_GAME |
| 4 | POST_GAME |
| 5 | MIGRATING |
| 6 | DESTRUCTING |
| 7 | RESETABLE |
| 9 | UNRESPONSIVE |
| 0x10 | GAME_GROUP_INITIALIZED |

**No hay `REPLAY_SETUP` (8).** Ese valor existe en Blaze2/3SDK, pero no en el enum de UFC: no
hay que mandarlo. El string `REPLAY_SETUP` que aparece en el binario es `trigger_REPLAY_SETUP` del
público, ajeno a Blaze.

### 2.3 `Game::replayGame` del SDK cliente (`0xf22840`)

```
f22868  cmpl $0x4,0x2c(%r13)        ; Game->gameState == POST_GAME ?
f2286d  jne  f2297e                 ;   no: error LOCAL 0x50004 (f229fe), no se manda nada
f22888  call eb9ed0                 ; ReplayGameRequest (vtable 0x24c0130); GID = game id
f228e8  mov  $0x13,%edx
f228f2  call f7eca0                 ; arma el RPC componente 4 / comando 0x13
f228fe  lea  e98c80,%rsi            ; callback interno: reenvía (error, Game*) al título
```

- El callback del título (`0xe98c80` → functor) recibe solo `(BlazeError, Game*)`. **No hay TDF de
  respuesta**, lo mismo que `RpcCommand<ReplayGameRequest, EmptyMessage>` en Blaze2/3SDK.
- El SDK **no cambia** `Game->gameState` cuando la respuesta sale bien. El estado local solo cambia
  con `NotifyGameStateChange`.
- `0x50004` es `GAMEMANAGER_ERR_INVALID_GAME_STATE_TRANSITION`. Lo decodifiqué del switch de nombres
  de error junto con estos códigos útiles:
  - `0x20004` INVALID_GAME_ID
  - `0x60004` INVALID_GAME_STATE_ACTION
  - `0x1e0004` PERMISSION_DENIED
  - `0x200004` NOT_IN_ADMIN_LIST

  Los errores de framework están en `0xf82480`: `0x40030000` ERR_COMMAND_NOT_FOUND,
  `0x40050000` **ERR_TIMEOUT** (`0xf82786 → 0xf82cc0`), `0x40060000` ERR_DISCONNECTED.

### 2.4 Por qué hace falta PRE_GAME: `Game::startGame` (`0xf22440`)

```
f22468  cmpl $0x82,0x2c(%r13)       ; Game->gameState == PRE_GAME ?
f22470  jne  f224c5                 ;   no: error local, sin RPC
f22472  mov  $0x83,%edx
f22480  call f28f70                 ;   sí: advanceGameState(IN_GAME)
```

Mismo campo `+0x2c`. Después de `replayGame`, el host vuelve a pedir `IN_GAME` (en la primera pelea lo
hizo a los 18 s de `PRE_GAME`). Solo lo consigue si el servidor llevó su `Game` local a `PRE_GAME`
mediante `0x64`.

### 2.5 Quién llama a `replayGame` y qué hace con el resultado (código UFC)

- `0xbc2200` (slot `+0x38` de la vtable `0x2490770`, capa de sesión de red de UFC):
  - si existe el `Game*` (`this+0x20`), llama a `0xbc1870(this+0x68)`;
  - después llama a `Game::replayGame` mediante `0xf227b0` → `0xf22840`, con el callback
    miembro `0xbc2280` (puntero en `0x25c42b0`).
- `0xbc2280(this, err, game)`:

  ```
  bc22ae  cmp $0x40050000,%esi ; setne al
  bc22b9  r14 = 2 + 4*(err != ERR_TIMEOUT)   ; 2 si timeout, 6 si otro error
  bc22c1  cmove %esi,%r14d                   ; 0 si éxito
  bc22d6  busca en la lista this+0x68 la operación con vf[0x10]() == 3
  bc22f2  op->vf[0x18](resultado)            ; y la saca de la lista
  ```

  Hoy, con `ERR_COMMAND_NOT_FOUND`, la operación de revancha termina con resultado 6. No encontré
  ningún reintento ni ninguna salida a menú en ese camino, y eso cuadra con "eligen Rematch y no pasa nada".

## 3. Negociación P2P de la revancha (no pasa por Blaze)

### 3.1 UI → controlador

Servicio `network:game` (despachador `0x2e2c10`). `S+0x50` es la interfaz en `C+0x38` del
controlador de juego en red (vtable `0x241adc8`; la vtable primaria es `0x241a938`).

| Comando UI | Llamada | Implementación |
|---|---|---|
| `get-rematch-status` | `vf[0x40]` | `0x35fc00`; devuelve `hostRematchStatus`, `clientRematchStatus` |
| `request-rematch {RematchAccepted}` | `vf[0x30]` | `0x35edb0` → `[C+0xe8]` → `0x372a70(R, accepted)` |
| `rematch-unload-completed` | `vf[0x38]` | `0x35ef20` → `0x35edd0`; devuelve `IsRematchActive` |
| `rematch-status-update-listener` | — | guarda el callback de la UI |

Strings de la UI en `0x1e68dcd..0x1e68eb8`: `update_rematch_selection`, `goto_online_rematch`,
`prepare/start/complete_gameplay_reset`.

### 3.2 Objeto de sesión `R = [C+0xe8]`

Vtable `0x241bff8`. Usa:

- `R+0x28`: link P2P;
- `R+0x38`: settings (`+0x2c` = 0 si es host);
- votos en `settings+0x110` (host) y `+0x114` (guest).

**`0x372a70 request-rematch`.** Sigue solo si el link puede enviar y el voto local todavía es 0.

- Guest: manda el paquete `0x15`, de 2 bytes: `[0x15, accepted]`.
- Host: manda el paquete `0x14`, de 3 bytes: `[0x14, hostVote, clientVote]`. Si el envío sale bien,
  guarda su voto y avisa al listener (`R+0x40 → vf[0xf0](host, client)`).

**`0x372690`, receptor.** Tabla `0x1e70c30`; el tipo del paquete es el primer byte.

| Tipo | Lado | Efecto |
|---|---|---|
| `0x14` (`0x3727f0`) | guest | copia los dos votos (`+0x110`/`+0x114`) y avisa a la UI |
| `0x15` (`0x37285c`) | host | contesta con `0x14 [hostVote, clientVote]`; si sale, guarda el voto del guest y avisa a la UI |

Constructores de los paquetes: `0x3772b0` (`0x14`) y `0x3772c0` (`0x15`). El `0x13` de 0xc808 bytes
(`0x377290`) es otro mensaje de este mismo canal, no la revancha.

**`0x35edd0 rematch-unload-completed`.** Pide que `R` esté en estado `0x14` (`0x371d20`). Después
**rearma el link de pregame sobre el mismo peer de ConnApi**:

- `0x374900` (el `model->link` del informe de pregame);
- un `GameLink` nuevo (`0xbdb8c0`) con stream `'fctn'`.

No hace ningún pedido Blaze. **La malla y el roster de la sala siguen vivos.** No hace falta
`NotifyGameSetup`, `PlayerJoining` ni repetir `finalizeGameCreation`.

### 3.3 Secuencia completa esperada

1. El host pide `advanceGameState(POST_GAME)` → `0x64 GSTA=4` a los dos. (Ya funciona.)
2. Los dos votan Rematch por P2P (`0x15`/`0x14`). (Ya funciona; no hay rastro en Blaze, como se
   espera.)
3. El host, con `Game` en `POST_GAME`, manda `replayGame {GID}`.
4. **Servidor:** responde vacío, y la sala y los dos clientes pasan a `PRE_GAME` (`0x64 GSTA=0x82`).
5. El código UFC recibe resultado 0, descarga la pelea y llama a `rematch-unload-completed` → rearma
   el link → pregame otra vez (P2P `0x11`, igual que en la primera pelea).
6. El host pide `advanceGameState(IN_GAME 0x83)` (`startGame`, que exige `PRE_GAME` local).

Los pasos 5 y 6 se infieren del código. No hay una corrida que los muestre.

## 4. Propuesta mínima para el servidor (la implementa Claude2 en `tools/ufc3_games.py`)

No toqué ese archivo. Contrato propuesto para `GAME_MANAGER 0x13 replayGame`:

1. Leer `GID` (INT). Resolver la sala con las mismas validaciones que ya tienen los demás comandos:
   - pertenencia;
   - conexión vigente;
   - **autoridad de host/admin** (`ADMN` = [host]).
   - Errores sugeridos: sala inexistente o no miembro → `0x20004` INVALID_GAME_ID; no admin →
     `0x1e0004` PERMISSION_DENIED.
2. Estado:
   - si la sala está en `POST_GAME (4)`: pasarla a `PRE_GAME (0x82)`;
   - si ya está en `PRE_GAME`: éxito vacío sin notificar (idempotente, como el mismo estado en
     `advanceGameState`);
   - cualquier otro estado: `0x50004` INVALID_GAME_STATE_TRANSITION, el mismo código que usa el SDK
     cliente.

   La tabla de transiciones actual ya permite `POST_GAME → PRE_GAME`, así que conviene reutilizarla.
3. Respuesta: **cuerpo vacío** (`b''`).
4. Notificación: `NotifyGameStateChange 0x64 {GID, GSTA=0x82}` a **todos** los jugadores de la sala,
   incluido el host que pidió.
5. No tocar:
   - roster, estados de jugador (siguen en 4), `finalized`, links de malla ni atributos;
   - no mandar `NotifyGameReset (0x70)`, `NotifyGameSetup` ni `PlatformHostInitialized`.
6. Opcional, prioridad baja: dar un `GSID`/reporting id nuevo y notificar
   `NotifyGameReportingIdChange 0x71 {GID, GRID}` a los dos.
   - **A favor:** PocketRelay lo hace en `replay()` (`src/services/game/mod.rs:271-279`).
   - **En contra / riesgo:** `GameReporting (0x1c)` hoy no tiene handler, así que no bloquea nada.
     Sin eso, el segundo informe de resultado reutiliza `GRID 2`.
   - Recomiendo dejarlo afuera del primer intento y agregarlo solo si la segunda pelea muestra
     problemas con los informes.

**Orden respuesta/notificación.** `ufc3_server.py` escribe primero la respuesta y después las
notificaciones (líneas 359–360). El callback de éxito de UFC no llama `startGame` en el acto: antes
descarga y vuelve a pasar por pregame, y ahí el `0x64` ya llegó en el mismo lote TCP. No debería
importar. Si en la prueba el host fallara con un error local de transición al iniciar la segunda
pelea, el primer cambio sería mandar el `0x64` antes que la respuesta. Ese cambio va en
`ufc3_server.py`, fuera del alcance de Claude2.

**Referencias que coinciden.**

- PocketRelay `handle_replay_game` (`src/session/routes/game_manager.rs:767`): `replay()` →
  `set_state(PreGame)` + reporting id, y responde vacío.
- BF4BlazeEmulator `replayGame` (`Components_Server/Game.py:349`): respuesta vacía +
  `0x64 GSTA=130`.

**Tests sugeridos** (los escribe Claude2):

- host en `POST_GAME` → respuesta vacía y dos `0x64` con `GSTA=0x82`, y la sala queda en `PRE_GAME`;
- guest → PERMISSION_DENIED, sin notificaciones;
- estado `IN_GAME`/`PRE_GAME` → error de transición, o éxito sin notificar en el caso idempotente;
- `GID` ajeno → INVALID_GAME_ID;
- después del replay, `advanceGameState(IN_GAME)` del host se acepta (los jugadores siguen
  conectados y `finalized`).

## 5. Qué falta / no verificado

- **Sin corrida.** Nada de §3.3 pasos 5–6 se observó; se infiere del desensamblado. Para confirmar:
  repetir la prueba con el servidor nuevo y buscar en el log, en orden:
  - `0x13` aceptado;
  - dos `0x64 GSTA=130`;
  - otro intercambio P2P de pregame;
  - `advanceGameState GSTA=131`.
- **No rastreé la operación de tipo 3** (`vf[0x10]() == 3` en la lista `this+0x68` de `0xbc2200`) ni
  qué hace con los resultados 0, 2 y 6. Tampoco quién llama al slot `+0x38` de la vtable
  `0x2490770`. Sé que el resultado de error no reintenta en `0xbc2280`; no sé si arriba se muestra
  algún mensaje.
- **Valores de voto** (`hostRematchStatus`/`clientRematchStatus`): no los mapeé. `0x371d50` escribe 2 en
  el voto local en una ruta de salida, así que probablemente 1 = acepta y 2 = rechaza o se fue. No afecta al servidor.
- **No confirmé si el guest necesita algo más** que el `0x64`. El guest no habla con Blaze en la
  revancha, y el rearmado del link (`0x35edd0`) es local. Si el guest se queda colgado y el host no,
  mirar el P2P (`0x14` con los dos votos), no Blaze.
- `NotifyGameReportingIdChange`: queda opcional (§4.6).
- Scripts usados (temporales, fuera del repo): `/tmp/ufc27_sw.py` (switches de nombres),
  `/tmp/ufc27_enum2.py` (enum GameState), `/tmp/ufc27_rip.py` + `/tmp/ufc27_q.py` (xrefs
  RIP-relativas), `/tmp/ufc27_call.py` (llamadas directas).
