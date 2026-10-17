// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
// ufc3: GCN code shares data between the lanes of a wave through LDS without barriers (a wave runs
// in lockstep, and s_waitcnt orders each lane's own LDS accesses): e.g. UFC 3's summed-area-table
// scans (cs 3bb24942) do a 32-lane Hillis-Steele scan with plain ds_write/ds_read. Plain SPIR-V
// loads and stores to Workgroup memory carry no such ordering, and the AMD driver (RX 9070) moved
// the reads of later steps before other lanes' writes: the scan restarted every 16 lanes and the
// octagon floor's soft shadows came out as grain and blocks. A subgroup-scope memory barrier
// around each LDS store keeps the guest's order (no hardware cost at wave scope).
// BB_LDS_ORDER=0 turns it off.
bool LdsOrdering() {
    static const bool on = [] {
        const char* e = std::getenv("BB_LDS_ORDER");
        return !(e && e[0] == '0');
    }();
    return on;
}
void LdsFence(EmitContext& ctx) {
    if (!LdsOrdering()) {
        return;
    }
    const auto semantics =
        spv::MemorySemanticsMask::AcquireRelease | spv::MemorySemanticsMask::WorkgroupMemory;
    ctx.OpMemoryBarrier(ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup)),
                        ctx.ConstU32(static_cast<u32>(semantics)));
}
} // namespace

Id EmitLoadSharedU16(EmitContext& ctx, Id offset) {
    const Id shift_id{ctx.ConstU32(1U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer = ctx.EmitSharedMemoryAccess(ctx.shared_u16, ctx.shared_memory_u16, index);
    return ctx.OpLoad(ctx.U16, pointer);
}

Id EmitLoadSharedU32(EmitContext& ctx, Id offset) {
    const Id shift_id{ctx.ConstU32(2U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer = ctx.EmitSharedMemoryAccess(ctx.shared_u32, ctx.shared_memory_u32, index);
    return ctx.OpLoad(ctx.U32[1], pointer);
}

Id EmitLoadSharedU64(EmitContext& ctx, Id offset) {
    const Id shift_id{ctx.ConstU32(3U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer = ctx.EmitSharedMemoryAccess(ctx.shared_u64, ctx.shared_memory_u64, index);
    return ctx.OpLoad(ctx.U64, pointer);
}

void EmitWriteSharedU16(EmitContext& ctx, Id offset, Id value) {
    const Id shift{ctx.ConstU32(1U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift)};
    const Id pointer = ctx.EmitSharedMemoryAccess(ctx.shared_u16, ctx.shared_memory_u16, index);
    LdsFence(ctx);
    ctx.OpStore(pointer, value);
    LdsFence(ctx);
}

void EmitWriteSharedU32(EmitContext& ctx, Id offset, Id value) {
    const Id shift{ctx.ConstU32(2U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift)};
    const Id pointer = ctx.EmitSharedMemoryAccess(ctx.shared_u32, ctx.shared_memory_u32, index);
    LdsFence(ctx);
    ctx.OpStore(pointer, value);
    LdsFence(ctx);
}

void EmitWriteSharedU64(EmitContext& ctx, Id offset, Id value) {
    const Id shift{ctx.ConstU32(3U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift)};
    const Id pointer = ctx.EmitSharedMemoryAccess(ctx.shared_u64, ctx.shared_memory_u64, index);
    LdsFence(ctx);
    ctx.OpStore(pointer, value);
    LdsFence(ctx);
}

} // namespace Shader::Backend::SPIRV
