set pagination off
set confirm off
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
starti
python
import gdb

def string(expr):
    try:
        return gdb.parse_and_eval('(const char*)(' + expr + ')').string()
    except Exception:
        return '<unreadable>'

class NucleusError(gdb.Breakpoint):
    def stop(self):
        print('ONLINE NUCLEUS ERROR', int(gdb.parse_and_eval('$esi')),
              string('$r8'), 'arg:', string('$r9'), flush=True)
        print(gdb.execute('bt 5', to_string=True), flush=True)
        if string('$r8').startswith('Location header'):
            print('ONLINE LOCATION HEADER COPY', repr(string('*(long*)($rdi+0xb0)')), flush=True)
            print('ONLINE PROTOHTTP HEADER', repr(string('*(long*)($rdi+0x188)+0x28')), flush=True)
            print('ONLINE PROTOHTTP HEADER LENGTH', int(gdb.parse_and_eval('*(int*)(*(long*)($rdi+0x188)+0x20)')), flush=True)
        return False

class AuthResponse(gdb.Breakpoint):
    def stop(self):
        print('ONLINE AUTH RESPONSE status=', int(gdb.parse_and_eval('*(int*)$rsi')),
              'location=', string('*(long*)($rsi+16)'), flush=True)
        return False

class AuthCode(gdb.Breakpoint):
    def stop(self):
        print('ONLINE AUTH CODE accepted=', string('*(long*)($rbx+0xb8)'), flush=True)
        return False

class RequestIssued(gdb.Breakpoint):
    def stop(self):
        obj = int(gdb.parse_and_eval('*(long*)($r15+0x30)'))
        print('ONLINE REQUEST ISSUED', int(gdb.parse_and_eval('$eax')),
              'nucleus object=%#x vtable=%#x' % (obj, int(gdb.parse_and_eval('*(long*)%d' % obj))), flush=True)
        return False

class RequestDone(gdb.Breakpoint):
    def stop(self):
        print('ONLINE NUCLEUS REQUEST DONE', hex(int(gdb.parse_and_eval('$rdi'))),
              'id=', int(gdb.parse_and_eval('*(int*)($rdi+8)')), flush=True)
        return False
class LoginFailure(gdb.Breakpoint):
    def stop(self):
        print('ONLINE LOGIN FAILURE error=%#x state=%d' % (
            int(gdb.parse_and_eval('$esi')) & 0xffffffff,
            int(gdb.parse_and_eval('*(int*)($rdi+0x98)'))), flush=True)
        print(gdb.execute('bt 8', to_string=True), flush=True)
        return False

class NullNumber(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('$rbx')) != 0:
            return False
        print('ONLINE NULL NUMERIC CONFIG', flush=True)
        print(gdb.execute('info registers rdi rsi rdx rcx r8 r9 r12 r13 r14 r15 rbp rsp', to_string=True), flush=True)
        try:
            obj = int(gdb.parse_and_eval('*(unsigned long long*)($r12+0x20)'))
            vtable = int(gdb.parse_and_eval('*(unsigned long long*)%d' % obj))
            getter = int(gdb.parse_and_eval('*(unsigned long long*)%d' % (vtable+0x50)))
            print('ONLINE NULL GETTER', hex(obj), hex(vtable), hex(getter), flush=True)
        except Exception:
            pass
        frame = int(gdb.parse_and_eval('$rbp'))
        for _ in range(10):
            try:
                previous = int(gdb.parse_and_eval('*(unsigned long long*)%d' % frame))
                caller = int(gdb.parse_and_eval('*(unsigned long long*)%d' % (frame + 8)))
                print('ONLINE FRAME', hex(frame), hex(caller), flush=True)
                if previous <= frame or previous - frame > 1048576:
                    break
                frame = previous
            except Exception:
                break
        return True

class LoginState(gdb.Breakpoint):
    def stop(self):
        state = int(gdb.parse_and_eval(self.expression))
        name = string('*(long*)(0x80248ff00+8*%d)' % state)
        print('ONLINE LOGIN STATE', state, name, flush=True)
        print('ONLINE STATE REGISTERS', gdb.execute('info registers rsi rdx rax rbx', to_string=True), flush=True)
        print(gdb.execute('bt 4', to_string=True), flush=True)
        if state == 19:
            self.enabled = False
            login_start.enabled = True
        return False

class LoginStart(gdb.Breakpoint):
    tracing = False
    def stop(self):
        address = int(gdb.parse_and_eval('$rdi')) + 0x98
        watch = LoginState('*(int*)%#x' % address, type=gdb.BP_WATCHPOINT,
                          wp_class=gdb.WP_WRITE)
        print('ONLINE LOGIN STATE WATCH', hex(address), flush=True)
        if not LoginStart.tracing:
            RequestDone('*0x801723de0')
            NucleusError('*0x801723d20')
            null_number = NullNumber('*0x8000402f0')
            null_number.condition = '$rbx == 0'
            LoginStart.tracing = True
        self.enabled = False
        return False

class AuthCompletion(gdb.Breakpoint):
    def stop(self):
        error = int(gdb.parse_and_eval('$esi')) & 0xffffffff
        print('ONLINE AUTH COMPLETION error=%#x' % error, flush=True)
        print(gdb.execute('bt 8', to_string=True), flush=True)
        return False

AuthCompletion('*0x800bb9b80', type=gdb.BP_HARDWARE_BREAKPOINT)
RequestIssued('*0x800bb3518', type=gdb.BP_HARDWARE_BREAKPOINT)
login_start = LoginStart('*0x800bb8eb0', type=gdb.BP_HARDWARE_BREAKPOINT)
end
continue
