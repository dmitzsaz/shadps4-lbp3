// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include "common/types.h"
#ifdef _WIN32
#include <malloc.h>
#endif

namespace Xbyak {
class CodeGenerator;
}

namespace Libraries::Fiber {
struct OrbisFiberContext;
}

namespace Core {

union DtvEntry {
    std::size_t counter;
    u8* pointer;
};

// A DTV belongs to the current guest thread. Module loading only appends slots;
// existing TLS blocks remain alive until that thread exits (unloading is unsupported).
// A stale generation, new slot or unallocated block must use the linker's locked path.
inline u8* TryGetTlsAddress(const DtvEntry* dtv, u32 generation, u64 module_index,
                            u64 offset) noexcept {
    if (dtv[0].counter != generation || module_index == 0 || module_index > dtv[1].counter) {
        return nullptr;
    }
    u8* address = dtv[module_index + 1].pointer;
    return address ? address + offset : nullptr;
}

struct Tcb {
    Tcb* tcb_self;
    DtvEntry* tcb_dtv;
    void* tcb_thread;
    ::Libraries::Fiber::OrbisFiberContext* tcb_fiber;
};

#ifdef _WIN32
/// Gets the thread local storage key for the TCB block.
u32 GetTcbKey();
#endif

/// Sets the data pointer to the TCB block.
void SetTcbBase(void* image_address);

/// Retrieves Tcb structure for the calling thread.
Tcb* GetTcbBase();

/// Makes sure TLS is initialized for the thread before entering guest.
void InitializeTLS();

template <auto f>
struct HostCallWrapperImpl;

template <class ReturnType, class... Args, PS4_SYSV_ABI ReturnType (*func)(Args...)>
struct HostCallWrapperImpl<func> {
    static ReturnType PS4_SYSV_ABI wrap(Args... args) {
        return func(args...);
    }
};

#define HOST_CALL(func) (Core::HostCallWrapperImpl<func>::wrap)

} // namespace Core
