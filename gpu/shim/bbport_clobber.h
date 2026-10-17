// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire issue #2, the clobbered dword: a single dword of a submitted command buffer (a PM4
// header, or a shader address register's payload) turns into 1. Always-on, cheap diagnostics
// that tell where the 1 arrives, and an opt-in trap for a host-side writer:
//
// - FirstBadHeader: the packet walk Liverpool::SubmitGfx runs over the copy it has just made
//   (= the guest buffer at submit) and ProcessGraphics runs again when it starts decoding it.
//   With the guest dword ReportBadPacket reads when it meets the bad header, that is three
//   readings per clobber: guest at submit, copy at decode, guest at decode.
// - NoteWrite: every write the port makes into guest memory on the game's behalf (fence labels,
//   WriteData, DMA, downloads) goes into a ring, and is reported at once when it lands inside
//   the range of a recently submitted command buffer (NoteSubmit). DumpWritesInto lists the
//   remembered writes into the clobbered dword.
// - BB_COPY_PROTECT=1: the host copies are allocated in whole pages and made read-only once
//   filled (ProtectCopy); a write into one faults, OnFault names the copy and the offset, and
//   the runtime's fault handler prints the writer's call chain and exits.
#pragma once

#include <cstddef>
#include <cstdint>

namespace BbClobber {
enum Source : std::uint32_t {
    Eop,
    Eos,
    EosGds,
    ReleaseMem,
    WriteData,
    ComputeWriteData,
    DmaFill,
    DmaCopy,
    BufferDownload,
    ImageDownload,
    ConstRam,
};

/// Dword offset of the first dword that is neither a type-3 packet header nor type-2 padding
/// when the buffer is walked packet by packet; -1 when the whole buffer parses.
std::int64_t FirstBadHeader(const std::uint32_t* words, std::uint64_t dwords);

/// Liverpool::SubmitGfx: a graphics submission's dcb in guest memory.
void NoteSubmit(std::uint64_t seq, const void* guest_dcb, std::uint64_t bytes);
/// GPU thread: the decoding of submission `seq` starts.
void NoteDecoding(std::uint64_t seq);
/// A write of `bytes` bytes to guest address `address` the port is about to make (`value`: the
/// first 8 bytes of what it writes). Reported when it lands inside a submitted command buffer.
void NoteWrite(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source);
/// The remembered writes that touched [address, address + bytes), newest first, on stderr.
void DumpWritesInto(std::uint64_t address, std::uint64_t bytes);

/// The late-label guard, before a fence label (EOP, EOS, RELEASE_MEM) is stored: true when the
/// write must be dropped because the game has rebuilt the command buffer that holds the slot
/// (the writer of SacredFire issue #2: the label of a submission written seconds later, by the
/// draw recording thread, over a packet of the next frame built into the same buffer).
/// `at_submit` (nullable): the slot's dwords as the buffer was submitted, read from the copy
/// when the packet was decoded. The game resets a label it waits on before it submits the
/// buffer and leaves it alone until the port writes it, so a slot that differs from its
/// submitted value, other than by a write of the port's own, has been rebuilt. Without
/// `at_submit` (a label outside the buffer being decoded, a compute queue), a slot inside a
/// submitted command buffer that holds a plausible PM4 type-3 header is taken as rebuilt.
/// Reports the write, the buffer, and the writer's call chain. `packet_seq`: the graphics
/// submission whose packet asked for the write (0: a compute queue or unknown).
bool GuardLabel(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source,
                std::uint64_t packet_seq, const std::uint32_t* at_submit);

/// ufc3 (UFC-47): the GPU thread's WAIT_REG_MEM on guest memory `address` has not been met for
/// `seconds` (it waits for (value & mask) <function> ref; function: the PM4 code). Once per
/// run, on stderr, with the value there now and the last label the guard dropped: a label the
/// game waits on that never arrives freezes the game here.
void ReportLongWait(std::uint64_t address, std::uint32_t function, std::uint32_t ref,
                    std::uint32_t mask, unsigned seconds);

/// BB_COPY_PROTECT=1.
bool ProtectCopies();
/// Storage for a SubmittedCopy: whole pages, page-aligned, when ProtectCopies(); malloc otherwise.
void* AllocCopy(std::size_t bytes);
void FreeCopy(void* storage, std::size_t bytes);
/// After the copy is filled: read-only until FreeCopy (ProtectCopies() only).
void ProtectCopy(std::uint64_t seq, const void* storage, std::size_t bytes);
/// At a SIGSEGV: true when `address` is inside a protected copy (the write is reported; the
/// caller lets the fault be fatal so that the writer's call chain is printed).
bool OnFault(void* ucontext, std::uint64_t address);

/// The bug just showed itself (a bad header, a changed copy, a late label, a trap): once per
/// run, a message on stderr that names GitHub issue #2 and asks for the log to be attached
/// to it. The launcher watches for the words "SacredFire issue #2" and shows a notice.
/// BB_ISSUE2_TEST=1 prints it at the first submission (to try the launcher's notice).
void AskForReport();
} // namespace BbClobber
