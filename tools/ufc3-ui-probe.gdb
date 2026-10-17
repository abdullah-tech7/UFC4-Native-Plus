# Probe gdb de pregame online (UFC-20, online paso 5a/6). Confirma las hipótesis
# de la sección 5 de ONLINE-PREGAME-RE.md con las direcciones de pregame: las del
# handoff viejo eran de Ultimate Team y no aplican.
#
# Hardware breakpoints (máximo 4), elegidos por rol con UFC3_PROBE_ROLE:
#   client: 0x3750ae  byte 3 del paquete 0x11 recibido (hostReadyState; el
#                     cliente descarta si <= 0) y 0x2e68b0 (data-change-listener).
#   host:   0x374a60  sendPregameData (link model+0x18, byte model+0x58 =
#                     uiInitialized/hostReadyState) y 0x2e5df0 (despachador
#                     network:pregame, comando en rdx).
# Las direcciones son offsets de la imagen guest con base 0x800000000.
#
# Uso (una PC de cada rol, sin frenar el juego: cada hit imprime y sigue):
#   BB_PROBE=tools/ufc3-ui-probe.sh UFC3_PROBE_ROLE=host   ./run.sh
#   BB_PROBE=tools/ufc3-ui-probe.sh UFC3_PROBE_ROLE=client ./run.sh
# Sin UFC3_PROBE_ROLE se asume client. Log en out/ufc3-ui-probe.log.
#
# No usar `handle SIGTRAP nopass`: rompe el single-step del runtime
# (docs/ufc3/ONLINE_IMPLEMENTATION.md).
set pagination off
set confirm off
set logging file out/ufc3-ui-probe.log
set logging overwrite on
set logging enabled on
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIGALRM nostop noprint pass
starti
python
import gdb, os

BASE = 0x800000000

def read_memory(addr, count):
    try:
        return gdb.selected_inferior().read_memory(addr, count).tobytes()
    except Exception:
        return None

def cstring(addr):
    # Lee hasta el primer NUL: `string(length=...)` traía NULs embebidos y el
    # print de Python fallaba con "embedded null character".
    if addr < 0x10000:
        return '<null %#x>' % addr
    try:
        out = gdb.execute('x/s %#x' % addr, to_string=True)
        return out.split(':\t', 1)[-1].strip()
    except Exception:
        raw = read_memory(addr, 200)
        if raw is None:
            return '<unreadable %#x>' % addr
        return raw.split(b'\0', 1)[0].decode('latin-1', 'replace')

def reg(name):
    try:
        return int(gdb.parse_and_eval('(unsigned long)$' + name))
    except Exception:
        return 0

class PregamePoint(gdb.Breakpoint):
    def __init__(self, name, offset):
        super().__init__('*%#x' % (BASE + offset),
                         type=gdb.BP_HARDWARE_BREAKPOINT)
        self.name = name
        self.hits = 0

    def stop(self):
        self.hits += 1
        try:
            detail = self.detail()
        except Exception as error:
            detail = '<inspection failed: %s>' % error
        print('PREGAME', self.name, 'hit', self.hits, detail, flush=True)
        return False

class RecvPacket(PregamePoint):
    # 0x3750ae: mov al,[r14+3] — byte 3 = hostReadyState.
    def detail(self):
        packet = reg('r14')
        raw = read_memory(packet, 13)
        if raw is None:
            return 'packet=%#x <unreadable>' % packet
        return 'packet=%#x bytes=%s hostReadyState=%d' % (
            packet, raw.hex(' '), raw[3])

class Listener(PregamePoint):
    # 0x2e68b0: notificación del data-change-listener a la UI.
    def detail(self):
        return 'rdi=%#x rsi=%#x rdx=%#x' % (
            reg('rdi'), reg('rsi'), reg('rdx'))

class SendPregameData(PregamePoint):
    # 0x374a60: sendPregameData(model) — model llega en rdi. No envía si el
    # link (model+0x18) es nulo; el byte 3 del paquete sale de model+0x58.
    def detail(self):
        model = reg('rdi')
        link = read_memory(model + 0x18, 8)
        state = read_memory(model + 0x58, 1)
        return 'model=%#x link=%#x hostReadyState(+0x58)=%s' % (
            model,
            int.from_bytes(link, 'little') if link else 0,
            state[0] if state else '<unreadable>')

class Dispatcher(PregamePoint):
    # 0x2e5df0: despachador network:pregame — comando en rdx.
    def detail(self):
        return 'command=%s' % cstring(reg('rdx'))

ROLE = os.environ.get('UFC3_PROBE_ROLE', 'client').strip().lower()
makers = {
    'client': (lambda: RecvPacket('recv-0x11', 0x3750ae),
               lambda: Listener('data-change-listener', 0x2e68b0)),
    'host': (lambda: SendPregameData('sendPregameData', 0x374a60),
             lambda: Dispatcher('network:pregame', 0x2e5df0)),
}
if ROLE not in makers:
    print('PREGAME unknown UFC3_PROBE_ROLE=%r, using client' % ROLE, flush=True)
    ROLE = 'client'
for make in makers[ROLE]:
    make()
print('PREGAME probe role=%s: breakpoints installed' % ROLE, flush=True)
end
continue
