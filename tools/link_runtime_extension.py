# SPDX-License-Identifier: GPL-2.0-or-later
"""Statically link one Clang COFF extension into an identified native runtime PE.

Preserves the complete UFC3 v0.1.5 renderer and links UFC4 service/thread adapters
using retained COFF symbols. Disables its optional online resolver. No game
binary or renderer instructions are modified. Rejects unknown donors, missing
or ambiguous symbols, unsafe layouts, relocations and unexpected call sites.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import struct


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def coff(data, start=0, pe=False):
    machine, count, stamp, ptr, nsym, optsize, flags = struct.unpack_from('<HHIIIHH', data, start)
    if machine != 0x8664:
        raise ValueError('Expected AMD64 COFF')
    sections = []
    strings = ptr + nsym * 18
    for i in range(count):
        at = start + 20 + optsize + i * 40
        name, vsize, rva, size, raw, reloc, _, nr, _, traits = struct.unpack_from('<8sIIIIIIHHI', data, at)
        label = name.rstrip(b'\0').decode()
        if label.startswith('/'):
            offset = strings + int(label[1:])
            label = data[offset:data.index(0, offset)].decode()
        sections.append(dict(name=label, vsize=vsize, rva=rva, size=size,
                             raw=raw, reloc=reloc, nr=nr, traits=traits, header=at))
    strings = ptr + nsym * 18
    symbols = {}
    i = 0
    while i < nsym:
        at = ptr + i * 18
        lo, hi, value, section, typ, storage, aux = struct.unpack_from('<IIIhHBB', data, at)
        name = (data[at:at+8].split(b'\0')[0] if lo else
                data[strings+hi:data.index(0, strings+hi)]).decode('utf-8', 'replace')
        symbols[i] = dict(name=name, value=value, section=section, typ=typ, storage=storage)
        i += 1 + aux
    return sections, symbols, optsize


def link(original, object_file, output, expected_sha256):
    if output.resolve() in (original.resolve(), object_file.resolve()):
        raise ValueError('Output must be a separate file')
    raw = original.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError('Donor SHA-256 differs from the recorded runtime')
    data = bytearray(raw)
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Invalid PE')
    sections, symbols, optsize = coff(data, pe+4, True)
    opt = pe + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x20b:
        raise ValueError('Expected PE32+')
    image_base = struct.unpack_from('<Q', data, opt+24)[0]
    section_align, file_align = struct.unpack_from('<II', data, opt+32)
    headers_end = pe + 24 + optsize + (len(sections)+2) * 40
    if headers_end > min(s['raw'] for s in sections if s['raw']):
        raise ValueError('Not enough header space for two extension sections')
    original_symbols = defaultdict(set)
    for sym in symbols.values():
        if sym['section'] > 0:
            original_symbols[sym['name']].add(sections[sym['section']-1]['rva'] + sym['value'])
    def old_symbol(name):
        values = original_symbols[name]
        if len(values) != 1:
            raise ValueError(f'Missing or ambiguous original symbol: {name}')
        return next(iter(values))
    obj = object_file.read_bytes()
    obj_sections, obj_symbols, _ = coff(obj)
    next_rva = align(max(s['rva'] + max(s['size'], s['vsize']) for s in sections), section_align)
    buffers = [bytearray(), bytearray()]
    offsets = {}
    for index, section in enumerate(obj_sections, 1):
        if not section['size'] and not section['vsize']:
            continue
        if section['name'] in ('.llvm_addrsig', '.drectve') or section['name'].startswith('.debug'):
            continue
        if section['name'] not in ('.text', '.rdata', '.data', '.bss'):
            raise ValueError(f'Unexpected COFF section: {section["name"]}')
        group = int(section['name'] in ('.data', '.bss'))
        buffer = buffers[group]
        offset = align(len(buffer), 16)
        buffer.extend(bytes(offset-len(buffer)))
        length = max(section['size'], section['vsize'])
        payload = obj[section['raw']:section['raw']+section['size']] if section['raw'] else bytes(length)
        buffer.extend(payload)
        if len(payload) < length:
            buffer.extend(bytes(length-len(payload)))
        offsets[index] = (group, offset)
    data_rva = align(next_rva + len(buffers[0]), section_align)
    bases = [next_rva, data_rva]
    def resolve(symbol):
        if symbol['section'] > 0:
            group, offset = offsets[symbol['section']]
            return bases[group] + offset + symbol['value']
        if symbol['section'] == -1:
            return symbol['value']
        return old_symbol(symbol['name'])
    fixups = []
    for index, section in enumerate(obj_sections, 1):
        if index not in offsets:
            continue
        group, offset = offsets[index]
        for i in range(section['nr']):
            location, sym_index, kind = struct.unpack_from('<IIH', obj, section['reloc'] + i*10)
            symbol_rva = resolve(obj_symbols[sym_index])
            at = offset + location
            site = bases[group] + at
            buffer = buffers[group]
            if 4 <= kind <= 9:
                addend = struct.unpack_from('<i', buffer, at)[0]
                value = addend + symbol_rva - (site + 4 + kind-4)
                struct.pack_into('<i', buffer, at, value)
            elif kind == 1:
                addend = struct.unpack_from('<Q', buffer, at)[0]
                struct.pack_into('<Q', buffer, at, image_base + symbol_rva + addend)
                fixups.append(site)
            elif kind == 3:
                addend = struct.unpack_from('<I', buffer, at)[0]
                struct.pack_into('<I', buffer, at, symbol_rva + addend)
            elif kind != 0:
                raise ValueError(f'Unsupported AMD64 relocation {kind}')
    extension = next(s for s in obj_symbols.values() if s['name'] == 'ufc4_bbgpu_fallback')
    extension_rva = resolve(extension)
    start, target = old_symbol('runtime_resolve'), old_symbol('bbgpu_resolve')
    end = min(v for values in original_symbols.values() for v in values if v > start)
    def file_offset(rva):
        for section in sections:
            if section['rva'] <= rva < section['rva']+section['size']:
                return section['raw'] + rva-section['rva']
        raise ValueError(f'RVA is not file-backed: {rva:x}')
    calls = []
    for rva in range(start, end-4):
        at = file_offset(rva)
        if data[at] in (0xe8, 0xe9) and rva+5+struct.unpack_from('<i', data, at+1)[0] == target:
            calls.append((rva, at))
    if len(calls) != 1:
        raise ValueError(f'Expected one GPU fallback call, found {len(calls)}')
    call_rva, call_offset = calls[0]
    before = bytes(data[call_offset:call_offset+5]).hex()
    struct.pack_into('<i', data, call_offset+1, extension_rva-call_rva-5)
    service_target=old_symbol('runtime_lookup')
    service_extension=resolve(next(s for s in obj_symbols.values() if s['name']=='ufc4_services_lookup'))
    service_calls=[]
    for rva in range(start,end-4):
        at=file_offset(rva)
        if data[at]==0xe8 and rva+5+struct.unpack_from('<i',data,at+1)[0]==service_target and data[at-8:at-3]==bytes.fromhex('ba63010000'):
            service_calls.append((rva,at))
    if len(service_calls)!=1: raise ValueError(f'Expected one services call, found {len(service_calls)}')
    service_call_rva,at=service_calls[0]
    struct.pack_into('<i',data,at+1,service_extension-service_call_rva-5)
    save_extension=resolve(next(s for s in obj_symbols.values() if s['name']=='ufc4_savedata_lookup'))
    save_calls=[]
    for rva in range(start,end-4):
        at=file_offset(rva)
        if data[at]==0xe8 and rva+5+struct.unpack_from('<i',data,at+1)[0]==service_target and data[at-8:at-3]==bytes.fromhex('ba15000000'):
            save_calls.append((rva,at))
    if len(save_calls)!=1: raise ValueError(f'Expected one savedata lookup call, found {len(save_calls)}')
    save_call_rva,at=save_calls[0]
    struct.pack_into('<i',data,at+1,save_extension-save_call_rva-5)
    kernel_extension=resolve(next(s for s in obj_symbols.values() if s['name']=='ufc4_kernel_lookup'))
    kernel_calls=[]
    for rva in range(start,end-4):
        at=file_offset(rva)
        if data[at]==0xe8 and rva+5+struct.unpack_from('<i',data,at+1)[0]==service_target and data[at-8:at-3]==bytes.fromhex('ba4c000000'):
            kernel_calls.append((rva,at))
    if len(kernel_calls)!=1: raise ValueError(f'Expected one kernel lookup call, found {len(kernel_calls)}')
    kernel_call_rva,at=kernel_calls[0]
    struct.pack_into('<i',data,at+1,kernel_extension-kernel_call_rva-5)
    thread_wrapper = next(s for s in obj_symbols.values() if s['name'] == 'ufc4_host_pthread_create')
    thread_wrapper_rva = resolve(thread_wrapper)
    create_start, pthread_target = old_symbol('create'), old_symbol('pthread_create')
    create_end = min(v for values in original_symbols.values() for v in values if v > create_start)
    thread_calls = []
    for rva in range(create_start, create_end-4):
        at = file_offset(rva)
        if data[at] == 0xe8 and rva+5+struct.unpack_from('<i', data, at+1)[0] == pthread_target:
            thread_calls.append((rva, at))
    if len(thread_calls) != 1:
        raise ValueError(f'Expected one guest pthread_create call, found {len(thread_calls)}')
    thread_call_rva, thread_call_offset = thread_calls[0]
    struct.pack_into('<i', data, thread_call_offset+1, thread_wrapper_rva-thread_call_rva-5)
    attach_name = next(n for n in original_symbols if n.startswith('attach.llvm.'))
    attach_target=old_symbol(attach_name)
    attach_extension=resolve(next(s for s in obj_symbols.values() if s['name']=='ufc4_thread_attach'))
    attach_calls=[]
    for caller in ('runtime_thread_current','host_start'):
        caller_start=old_symbol(caller)
        caller_end=min(v for values in original_symbols.values() for v in values if v>caller_start)
        matches=[]
        for rva in range(caller_start,caller_end-4):
            at=file_offset(rva)
            if data[at]==0xe8 and rva+5+struct.unpack_from('<i',data,at+1)[0]==attach_target:
                matches.append((rva,at))
        if len(matches)!=1: raise ValueError(f'Expected one attach call in {caller}, found {len(matches)}')
        rva,at=matches[0]
        struct.pack_into('<i',data,at+1,attach_extension-rva-5)
        attach_calls.append(hex(rva))
    if expected_sha256!='3d2bcd9b6b59d349e73c03b940cec01e4589c972ed4f29b0e524335a3377651a':
        raise ValueError('Only the measured v0.1.5 runtime is supported by this linker')
    # v0.1.5 contains BC-through-memory, DB_ALPHA_TO_MASK, NaN handling and
    # completed-label fixes. Keep the whole renderer unchanged; no v0.1.2
    # class offsets, old shader hooks, capture hooks or old counter guards.
    texture_patches=[];probe_patches=[];label_patches=[]
    offline=resolve(next(s for s in obj_symbols.values() if s['name']=='ufc4_offline_resolver'))
    net_target=old_symbol('runtime_net_resolve');net_calls=[]
    for rva in range(start,end-4):
        at=file_offset(rva)
        if data[at]==0xe8 and rva+5+struct.unpack_from('<i',data,at+1)[0]==net_target:
            net_calls.append((rva,at))
    if len(net_calls)!=1:raise ValueError('Expected one optional online resolver call')
    net_rva,net_at=net_calls[0]
    if data[net_at-3:net_at]!=bytes.fromhex('4889f1'):raise ValueError('Offline resolver ABI mismatch')
    struct.pack_into('<i',data,net_at+1,offline-net_rva-5)
    # Preserve ASLR for pointer relocations emitted by the extension, when present.
    if fixups:
        reloc_rva, reloc_size = struct.unpack_from('<II', data, opt+112+5*8)
        inherited = raw[file_offset(reloc_rva):file_offset(reloc_rva)+reloc_size]
        blocks = defaultdict(list)
        for rva in fixups:
            blocks[rva & ~4095].append(0xa000 | (rva & 4095))
        additional = bytearray(inherited)
        for page, entries in sorted(blocks.items()):
            if len(entries) % 2:
                entries.append(0)
            additional.extend(struct.pack('<II', page, 8+2*len(entries)))
            additional.extend(struct.pack('<'+'H'*len(entries), *entries))
        offset = align(len(buffers[1]), 4)
        buffers[1].extend(bytes(offset-len(buffers[1])))
        buffers[1].extend(additional)
        struct.pack_into('<II', data, opt+112+5*8, data_rva+offset, len(additional))
    for group, (name, traits) in enumerate(((b'.u4text', 0x60000060), (b'.u4data', 0xc0000040))):
        if not buffers[group]:
            buffers[group].extend(bytes(16))
        offset = align(len(data), file_align)
        data.extend(bytes(offset-len(data)))
        size = align(len(buffers[group]), file_align)
        data.extend(buffers[group])
        data.extend(bytes(size-len(buffers[group])))
        header = pe+24+optsize+(len(sections)+group)*40
        struct.pack_into('<8sIIIIIIHHI', data, header, name, len(buffers[group]), bases[group], size,
                         offset, 0, 0, 0, 0, traits)
    struct.pack_into('<H', data, pe+6, len(sections)+2)
    struct.pack_into('<I', data, opt+56, align(data_rva+len(buffers[1]), section_align))
    struct.pack_into('<I', data, opt+64, 0)  # PE checksum; not required for a user executable
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    report = dict(donor_sha256=expected_sha256, object_sha256=hashlib.sha256(obj).hexdigest(),
                  output_sha256=hashlib.sha256(data).hexdigest(), extension_rva=hex(extension_rva),
                  fallback_call_rva=hex(call_rva), original_call_bytes=before,
                  guest_thread_create_call_rva=hex(thread_call_rva),
                  thread_attach_call_rvas=attach_calls,
                  services_resolver_call_rva=hex(service_call_rva),
                  savedata_resolver_call_rva=hex(save_call_rva),
                  kernel_resolver_call_rva=hex(kernel_call_rva),
                  texture_sync_patches=texture_patches,
                  material_probe_patches=probe_patches,
                  gpu_label_patches=label_patches,
                  offline_resolver_call_rva=hex(net_rva),
                  renderer_base='ufc3-v0.1.5; native BC-through-memory and DB_ALPHA_TO_MASK; no renderer bytes changed',
                  extension_bytes=[len(b) for b in buffers], absolute_pointer_relocations=len(fixups),
                  method='Measured v0.1.5 donor; UFC4 service/thread adapters and forced offline resolver; complete donor renderer preserved; game files unmodified.')
    output.with_suffix('.link.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--donor', type=Path, required=True)
    p.add_argument('--object', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--sha256', required=True)
    a = p.parse_args()
    if a.out.resolve() in (a.donor.resolve(), a.object.resolve()):
        p.error('Output must be a separate file')
    link(a.donor, a.object, a.out, a.sha256)
