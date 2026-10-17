# SPDX-License-Identifier: GPL-2.0-or-later
"""Blaze 15 wire format for the UFC 3 online server: Fire2 frames and TDF (heat2) values.

Fire2 header (16 bytes, big endian): payload size u32, metadata size u16, component u16,
command u16, message number u24, message type (top 3 bits) u8, options u8, reserved u8.

TDF fields: tag (4 upper-case characters, 6 bits each after -0x20, packed in 3 bytes), type byte,
value. Integers are variable length: first byte 0x80 continuation, 0x40 negative, 6 value bits;
then 7 bits per byte.
"""
import struct

# Heat2 wire type. TimeValue is 0xB (one signed varint, microseconds). The eboot's
# typeinfo id for that class is 0xE; that id is not the byte on the wire.
INT, STRING, BLOB, STRUCT, LIST, MAP, UNION, VARIABLE, OBJTYPE, OBJID, FLOAT, TIME, GENERIC = (
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xB, 12)


# Message types (top 3 bits of the type byte).
MESSAGE, REPLY, NOTIFICATION, ERROR_REPLY, PING, PING_REPLY = 0, 1, 2, 3, 4, 5


def encode_tag(tag: str) -> bytes:
    tag = (tag + '    ')[:4]
    v = 0
    for ch in tag:
        v = (v << 6) | ((ord(ch) - 0x20) & 0x3f)
    return v.to_bytes(3, 'big')


def decode_tag(b: bytes) -> str:
    v = int.from_bytes(b[:3], 'big')
    return ''.join(chr(((v >> s) & 0x3f) + 0x20) for s in (18, 12, 6, 0)).rstrip()


def encode_int(value: int) -> bytes:
    negative = value < 0
    v = -value if negative else value
    first = (v & 0x3f) | (0x40 if negative else 0)
    v >>= 6
    out = bytearray()
    if v:
        first |= 0x80
    out.append(first)
    while v:
        b = v & 0x7f
        v >>= 7
        out.append(b | (0x80 if v else 0))
    return bytes(out)


def decode_int(data: bytes, pos: int):
    b = data[pos]; pos += 1
    negative = bool(b & 0x40)
    v = b & 0x3f
    shift = 6
    more = b & 0x80
    while more:
        b = data[pos]; pos += 1
        v |= (b & 0x7f) << shift
        shift += 7
        more = b & 0x80
    return (-v if negative else v), pos


class Struct(dict):
    """An ordered TDF struct: tag -> (type, value)."""


class Union:
    def __init__(self, member=0x7f, tag=None, type_=None, value=None):
        self.member, self.tag, self.type, self.value = member, tag, type_, value

    def __repr__(self):
        return f'Union({self.member}, {self.tag}={self.value!r})' if self.member != 0x7f else 'Union(unset)'


def _encode_value(t: int, v) -> bytes:
    if t in (INT, TIME):
        return encode_int(int(v))
    if t == STRING:
        raw = v.encode('utf-8') if isinstance(v, str) else bytes(v)
        return encode_int(len(raw) + 1) + raw + b'\0'
    if t == BLOB:
        raw = bytes(v)
        return encode_int(len(raw)) + raw
    if t == STRUCT:
        return encode_struct(v) + b'\0'
    if t == LIST:
        elem_type, items = v
        out = bytes([elem_type]) + encode_int(len(items))
        for item in items:
            out += _encode_value(elem_type, item)
        return out
    if t == MAP:
        key_type, value_type, pairs = v
        out = bytes([key_type, value_type]) + encode_int(len(pairs))
        for key, value in pairs:
            out += _encode_value(key_type, key) + _encode_value(value_type, value)
        return out
    if t == UNION:
        if v is None or v.member == 0x7f:
            return b'\x7f'
        return bytes([v.member]) + encode_tag(v.tag) + bytes([v.type]) + _encode_value(v.type, v.value)
    if t == OBJTYPE:
        return encode_int(v[0]) + encode_int(v[1])
    if t == OBJID:
        return encode_int(v[0]) + encode_int(v[1]) + encode_int(v[2])
    if t == FLOAT:
        return struct.pack('>f', float(v))
    raise ValueError(f'cannot encode TDF type {t}')


def encode_struct(fields) -> bytes:
    """fields: list of (tag, type, value) or a Struct."""
    items = fields.items() if isinstance(fields, dict) else ((t, (ty, v)) for t, ty, v in fields)
    out = bytearray()
    # Heat2 decoders walk a class's members in tag order and skip what they do not expect next:
    # a member written out of order is silently lost.
    for tag, (t, v) in sorted(items, key=lambda item: encode_tag(item[0])):
        out += encode_tag(tag) + bytes([t]) + _encode_value(t, v)
    return bytes(out)


def _decode_value(t: int, data: bytes, pos: int):
    if t in (INT, TIME):
        return decode_int(data, pos)
    if t in (STRING, BLOB):
        n, pos = decode_int(data, pos)
        if n < 0 or pos + n > len(data) or (t == STRING and n == 0):
            raise ValueError('invalid or truncated TDF string/blob')
        raw = data[pos:pos + n]
        pos += n
        if t == STRING:
            if not raw.endswith(b'\0'):
                raise ValueError('unterminated TDF string')
            return raw[:-1].decode('utf-8', 'strict'), pos
        return raw, pos
    if t == STRUCT:
        if pos < len(data) and data[pos] == 2:  # struct with a version prefix (heat2 "has base")
            pass
        return decode_struct(data, pos, nested=True)
    if t == LIST:
        elem_type = data[pos]; pos += 1
        n, pos = decode_int(data, pos)
        if n < 0 or n > len(data) - pos:
            raise ValueError('invalid TDF list length')
        items = []
        for _ in range(n):
            item, pos = _decode_value(elem_type, data, pos)
            items.append(item)
        return (elem_type, items), pos
    if t == MAP:
        key_type, value_type = data[pos], data[pos + 1]; pos += 2
        n, pos = decode_int(data, pos)
        if n < 0 or n > len(data) - pos:
            raise ValueError('invalid TDF map length')
        pairs = []
        for _ in range(n):
            k, pos = _decode_value(key_type, data, pos)
            v, pos = _decode_value(value_type, data, pos)
            pairs.append((k, v))
        return (key_type, value_type, pairs), pos
    if t == UNION:
        member = data[pos]; pos += 1
        if member == 0x7f:
            return Union(), pos
        tag = decode_tag(data[pos:pos + 3]); ty = data[pos + 3]; pos += 4
        value, pos = _decode_value(ty, data, pos)
        return Union(member, tag, ty, value), pos
    if t == VARIABLE:
        present = data[pos]; pos += 1
        if not present:
            return None, pos
        tdf_id, pos = decode_int(data, pos)
        value, pos = decode_struct(data, pos, nested=True)
        return ('variable', tdf_id, value), pos
    if t == OBJTYPE:
        a, pos = decode_int(data, pos); b, pos = decode_int(data, pos)
        return (a, b), pos
    if t == OBJID:
        a, pos = decode_int(data, pos); b, pos = decode_int(data, pos); c, pos = decode_int(data, pos)
        return (a, b, c), pos
    if t == FLOAT:
        return struct.unpack('>f', data[pos:pos + 4])[0], pos + 4
    if t == GENERIC:
        present = data[pos]; pos += 1
        if not present:
            return None, pos
        tdf_id, pos = decode_int(data, pos)
        if data[pos + 3] <= GENERIC:  # a tagged value with the tag '    '
            inner_type = data[pos + 3]; pos += 4
        else:
            # UFC (Blaze 15) writes the value untagged; its type comes from the tdf id. Scenario
            # attributes are lists of strings ("abstain", "0".."9").
            inner_type = LIST
        value, pos = _decode_value(inner_type, data, pos)
        if pos < len(data) and data[pos] == 0:
            pos += 1
        return ('generic', tdf_id, value), pos
    raise ValueError(f'unknown TDF type {t} at {pos}')


def decode_partial(data: bytes):
    """The top-level members before the first one this decoder cannot read (and the error)."""
    out, pos = Struct(), 0
    try:
        while pos < len(data):
            tag = decode_tag(data[pos:pos + 3])
            t = data[pos + 3]
            value, pos = _decode_value(t, data, pos + 4)
            out[tag] = (t, value)
    except Exception as error:  # noqa: BLE001 - report what was readable
        return out, error
    return out, None


def decode_struct(data: bytes, pos: int = 0, nested: bool = False):
    out = Struct()
    while pos < len(data):
        if nested and data[pos] == 0:
            return out, pos + 1
        tag = decode_tag(data[pos:pos + 3])
        t = data[pos + 3]
        pos += 4
        value, pos = _decode_value(t, data, pos)
        if tag in out:
            raise ValueError('duplicate TDF tag')
        out[tag] = (t, value)
    if nested:
        raise ValueError('unterminated TDF struct')
    return out, pos


def dump(fields, indent: int = 0) -> str:
    lines = []
    for tag, (t, v) in fields.items():
        pad = '  ' * indent
        if t == STRUCT:
            lines.append(f'{pad}{tag}: {{')
            lines.append(dump(v, indent + 1))
            lines.append(f'{pad}}}')
        elif t == LIST and v[0] == STRUCT:
            lines.append(f'{pad}{tag}: [')
            for item in v[1]:
                lines.append(dump(item, indent + 1))
                lines.append(f'{pad}  ,')
            lines.append(f'{pad}]')
        else:
            lines.append(f'{pad}{tag} ({t}): {v!r}')
    return '\n'.join(line for line in lines if line)


class Frame:
    def __init__(self, component, command, msg_num=0, msg_type=MESSAGE, payload=b'', options=0, metadata=b''):
        self.component, self.command, self.msg_num = component, command, msg_num
        self.msg_type, self.payload, self.options, self.metadata = msg_type, payload, options, metadata

    def encode(self) -> bytes:
        return (struct.pack('>IHHH', len(self.payload), len(self.metadata), self.component, self.command)
                + self.msg_num.to_bytes(3, 'big') + bytes([self.msg_type << 5, self.options, 0])
                + self.metadata + self.payload)

    def __repr__(self):
        kind = ['MESSAGE', 'REPLY', 'NOTIFY', 'ERROR', 'PING', 'PONG', '6', '7'][self.msg_type & 7]
        return f'Frame({kind} comp {self.component:#x} cmd {self.command:#x} num {self.msg_num} {len(self.payload)} B)'


def read_frames(buffer: bytearray):
    """Complete frames at the start of `buffer` (consumed)."""
    frames = []
    while len(buffer) >= 16:
        size, meta, comp, cmd = struct.unpack('>IHHH', buffer[:10])
        total = 16 + meta + size
        if len(buffer) < total:
            break
        num = int.from_bytes(buffer[10:13], 'big')
        frames.append(Frame(comp, cmd, num, buffer[13] >> 5, bytes(buffer[16 + meta:total]),
                            buffer[14], bytes(buffer[16:16 + meta])))
        del buffer[:total]
    return frames
