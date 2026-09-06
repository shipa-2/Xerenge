#pragma once

#include <cstdint>

// Burnout Revenge bring-up hooks for the generated PPC translation units.
//
// These are game-specific mid-asm hooks (see the [[midasm_hook]] entries in
// work/B4_pdb.toml) rather than anything the recompiler itself knows about.
// XenonRecomp's generated ppc_recomp.*.cpp files only include
// ppc_recomp_shared.h (ppc_config.h + ppc_context.h from the generic tool),
// so this header is force-included for the generated PPC translation units
// via the xerenge-ppc CMake target instead of leaking Burnout addresses and
// symbol names into the generic tool's own headers.
//
// Implementations live in XerengeRuntime/src/main.cpp, next to the
// PPCGuestClock/PPCMaterializeObject services they wrap.

struct PPCContext;
union PPCRegister;

extern "C" void PPCGuestClockMidAsmHook(PPCRegister& r3);
extern "C" void PPCStubZeroMidAsmHook(PPCRegister& r3);
extern "C" void PPCStubZeroClearOutputMidAsmHook(PPCRegister& r3, PPCRegister& r4, uint8_t* base);
extern "C" void PPCMaterializeIfZeroMidAsmHook(PPCRegister& r3, PPCContext& ctx, uint8_t* base);
