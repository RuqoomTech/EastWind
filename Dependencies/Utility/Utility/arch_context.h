/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Arch-neutral access to the registers in a Win32 CONTEXT, for crash dumps and stack walks.

#pragma once

#include <windows.h>

#if defined(_M_X64)
#define CTX_PC(ctx) ((ctx).Rip)
#define CTX_STACK(ctx) ((ctx).Rsp)
#define CTX_FRAME(ctx) ((ctx).Rbp)
#define CTX_AX(ctx) ((ctx).Rax)
#define CTX_BX(ctx) ((ctx).Rbx)
#define CTX_CX(ctx) ((ctx).Rcx)
#define CTX_DX(ctx) ((ctx).Rdx)
#define CTX_SI(ctx) ((ctx).Rsi)
#define CTX_DI(ctx) ((ctx).Rdi)
#define CTX_STACKWALK_MACHINE IMAGE_FILE_MACHINE_AMD64
#elif defined(_M_IX86)
#define CTX_PC(ctx) ((ctx).Eip)
#define CTX_STACK(ctx) ((ctx).Esp)
#define CTX_FRAME(ctx) ((ctx).Ebp)
#define CTX_AX(ctx) ((ctx).Eax)
#define CTX_BX(ctx) ((ctx).Ebx)
#define CTX_CX(ctx) ((ctx).Ecx)
#define CTX_DX(ctx) ((ctx).Edx)
#define CTX_SI(ctx) ((ctx).Esi)
#define CTX_DI(ctx) ((ctx).Edi)
#define CTX_STACKWALK_MACHINE IMAGE_FILE_MACHINE_I386
#else
#error "arch_context.h: unsupported architecture"
#endif
