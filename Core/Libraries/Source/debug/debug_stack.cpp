/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

/////////////////////////////////////////////////////////////////////////EA-V1
// $File: //depot/GeneralsMD/Staging/code/Libraries/Source/debug/debug_stack.cpp $
// $Author: KMorness $
// $Revision: #2 $
// $DateTime: 2005/01/19 15:02:33 $
//
// (c) 2003 Electronic Arts
//
// Stack walker
//////////////////////////////////////////////////////////////////////////////

#include "debug/debug.h"
#include "debug/debug_stack.h"
#include <windows.h>
#include "Utility/stringex.h"
#include <imagehlp.h>
#undef StackWalk
#include <stdio.h>
#include "Utility/arch_context.h"

// Definitions to allow run-time linking to the dbghelp.dll functions.

#define DBGHELP(name,ret,par) typedef ret (WINAPI *name##Type) par;
#include "debug_stack.inl"
#undef DBGHELP

#define DBGHELP(name,ret,par) name##Type _##name;
static union
{
  struct
  {
#include "debug_stack.inl"
  };
  uintptr_t funcPtr[1];
} gDbg;
#undef DBGHELP

#define DBGHELP(name,ret,par) #name,
static char const *const DebughelpFunctionNames[] =
{
#include "debug_stack.inl"
	nullptr
};
#undef DBGHELP

// local dbghelp.dll module handle
static HMODULE g_dbghelp;

// local flag that is true if we're using an old dbghelp.dll version
static bool g_oldDbghelp;

static void InitDbghelp()
{
  // already called?
  if (g_dbghelp)
    return;

	// System dbghelp first: the retail one in the game folder is old and 32-bit only
	char dbgHelpPath[MAX_PATH];
	UINT len=GetSystemDirectory(dbgHelpPath,sizeof(dbgHelpPath));
	if (len && len+sizeof("\\DBGHELP.DLL")<=sizeof(dbgHelpPath))
	{
		strcpy(dbgHelpPath+len,"\\DBGHELP.DLL");
		g_dbghelp=::LoadLibrary(dbgHelpPath);
	}
	if (!g_dbghelp)
		// load any version we can
		g_dbghelp=::LoadLibrary("DBGHELP.DLL");

  if (!g_dbghelp)
    return;

  // Get function addresses
  uintptr_t *funcptr=gDbg.funcPtr;
  unsigned k=0;
  for (;DebughelpFunctionNames[k];++k,++funcptr)
  {
    *funcptr=(uintptr_t)GetProcAddress(g_dbghelp,DebughelpFunctionNames[k]);
    if (!*funcptr)
      break;
  }
  if (DebughelpFunctionNames[k])
  {
    // not all functions found -> clear them all
    while (funcptr!=gDbg.funcPtr)
      *--funcptr=0;
  }
  else
  {
    // Set options
    gDbg._SymSetOptions(gDbg._SymGetOptions()|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);

    // Init module
    gDbg._SymInitialize((HANDLE)GetCurrentProcessId(),nullptr,TRUE);

    // Check: are we using a newer version of dbghelp.dll?
    // (older versions have some serious issues.. err... bugs)
    if (!GetProcAddress(g_dbghelp,"SymEnumSymbolsForAddr"))
      g_oldDbghelp=true;
  }
}

//////////////////////////////////////////////////////////////////////////////

DebugStackwalk::Signature::Signature(const Signature &src)
{
  *this=src;
}

DebugStackwalk::Signature& DebugStackwalk::Signature::operator=(const Signature& src)
{
  if (&src!=this)
  {
    m_numAddr=src.m_numAddr;
    memcpy(m_addr,src.m_addr,m_numAddr*sizeof(*m_addr));
  }
  return *this;
}

uintptr_t DebugStackwalk::Signature::GetAddress(int n) const
{
  DFAIL_IF_MSG(n<0||n>=MAX_ADDR,n << "/" << MAX_ADDR) return 0;
  return m_addr[n];
}

void DebugStackwalk::Signature::GetSymbol(uintptr_t addr, char *buf, unsigned bufSize)
{
  DFAIL_IF(!buf) return;
  DFAIL_IF(bufSize<64||bufSize>=0x80000000) return;

  InitDbghelp();

  char *bufEnd=buf+bufSize;
  *buf=0;
  buf+=sprintf(buf,"%p",(void*)addr);

  // determine module
  DWORD64 modBase=gDbg._SymGetModuleBase64((HANDLE)GetCurrentProcessId(),addr);
  if (!modBase)
	{
		strcpy(buf," (unknown module)");
    return;
	}

  // illegal code ptr?
	if (IsBadReadPtr((void *)addr,4)||IsBadCodePtr((FARPROC)addr))
	{
		strcpy(buf," (invalid code addr)");
		return;
	}

  char symbolBuffer[512];
  GetModuleFileName((HMODULE)(uintptr_t)modBase,symbolBuffer,sizeof(symbolBuffer));

  const char *p=strrpbrk(symbolBuffer,"/\\"); // use filename only, strip off path
  p=p?p+1:symbolBuffer;
  *buf++=' ';
  strcpy(buf,p);
  buf+=strlen(buf);
  if (bufEnd-buf<32)
    return;
  buf+=wsprintf(buf,"+0x%x",(unsigned)(addr-modBase));

  // determine symbol
  PIMAGEHLP_SYMBOL64 symPtr=(PIMAGEHLP_SYMBOL64)symbolBuffer;
  memset(symPtr,0,sizeof(symbolBuffer));
  symPtr->SizeOfStruct=sizeof(IMAGEHLP_SYMBOL64);
  symPtr->MaxNameLength=sizeof(symbolBuffer)-sizeof(IMAGEHLP_SYMBOL64);
  DWORD64 symDisplacement;
  if (!gDbg._SymGetSymFromAddr64((HANDLE)GetCurrentProcessId(),addr,&symDisplacement,symPtr))
    return;
  if ((unsigned int)(bufEnd-buf)<strlen(symPtr->Name)+16)
    return;
  buf+=wsprintf(buf,", %s+0x%x",symPtr->Name,(unsigned)symDisplacement);

  // and line number
  IMAGEHLP_LINE64 line;
  memset(&line,0,sizeof(line));
  line.SizeOfStruct=sizeof(line);
  DWORD displacement;
  if (!gDbg._SymGetLineFromAddr64((HANDLE)GetCurrentProcessId(),addr,&displacement,&line))
    return;

  p=strrpbrk(line.FileName,"/\\"); // use filename only, strip off path
  p=p?p+1:line.FileName;

  if ((unsigned int)(bufEnd-buf)<strlen(p)+16)
    return;
  buf+=wsprintf(buf,", %s:%i+0x%x",p,line.LineNumber,displacement);
}

void DebugStackwalk::Signature::GetSymbol(uintptr_t addr,
                                          char *bufMod, unsigned sizeMod, unsigned *relMod,
                                          char *bufSym, unsigned sizeSym, unsigned *relSym,
                                          char *bufFile, unsigned sizeFile, unsigned *linePtr, unsigned *relLine)
{
  InitDbghelp();

  if (bufMod) *bufMod=0;
  if (relMod) *relMod=0;
  if (bufSym) *bufSym=0;
  if (relSym) *relSym=0;

  if (bufFile) *bufFile=0;
  if (linePtr) *linePtr=0;
  if (relLine) *relLine=0;

  DFAIL_IF(bufMod&&sizeMod<16) return;
  DFAIL_IF(bufSym&&sizeSym<16) return;
  DFAIL_IF(bufFile&&sizeFile<16) return;

  // determine module
  DWORD64 modBase=gDbg._SymGetModuleBase64((HANDLE)GetCurrentProcessId(),addr);
  if (!modBase)
	{
    if (bufMod)
		  strcpy(bufMod,"(unknown mod)");
    if (bufSym)
      strcpy(bufSym,"(unknown)");
    return;
	}

  // illegal code ptr?
	if (IsBadReadPtr((void *)addr,4)||IsBadCodePtr((FARPROC)addr))
	{
    if (bufMod)
		  strcpy(bufMod,"(inv code addr)");
    if (bufSym)
      strcpy(bufSym,"(unknown)");
		return;
	}

  char symbolBuffer[512];
  if (bufMod)
  {
    GetModuleFileName((HMODULE)(uintptr_t)modBase,symbolBuffer,sizeof(symbolBuffer));

    const char *p=strrpbrk(symbolBuffer,"/\\"); // use filename only, strip off path
    p=p?p+1:symbolBuffer;
    strlcpy(bufMod,p,sizeMod);
  }
  if (relMod)
    *relMod=(unsigned)(addr-modBase);

  // determine symbol
  if (bufSym)
  {
    PIMAGEHLP_SYMBOL64 symPtr=(PIMAGEHLP_SYMBOL64)symbolBuffer;
    memset(symPtr,0,sizeof(symbolBuffer));
    symPtr->SizeOfStruct=sizeof(IMAGEHLP_SYMBOL64);
    symPtr->MaxNameLength=sizeof(symbolBuffer)-sizeof(IMAGEHLP_SYMBOL64);
    DWORD64 displacement;
    if (gDbg._SymGetSymFromAddr64((HANDLE)GetCurrentProcessId(),addr,&displacement,symPtr))
    {
      strlcpy(bufSym,symPtr->Name,sizeSym);
      if (relSym)
        *relSym=(unsigned)displacement;
    }
    else
      strcpy(bufSym,"(unknown)");
  }

  // and line number
  if (bufFile)
  {
    IMAGEHLP_LINE64 line;
    memset(&line,0,sizeof(line));
    line.SizeOfStruct=sizeof(line);
    DWORD displacement;
    if (!gDbg._SymGetLineFromAddr64((HANDLE)GetCurrentProcessId(),addr,&displacement,&line))
      strcpy(bufFile,"(unknown)");
    else
    {
      const char *p=strrpbrk(line.FileName,"/\\"); // use filename only, strip off path
      p=p?p+1:line.FileName;
      strlcpy(bufFile,p,sizeFile);
      if (linePtr)
        *linePtr=line.LineNumber;
      if (relLine)
        *relLine=displacement;
    }
  }
}

Debug& operator<<(Debug &dbg, const DebugStackwalk::Signature &sig)
{
  dbg << sig.Size() << " addresses:\n";

  for (unsigned k=0;k<sig.Size();k++)
  {
    char buf[512];
    sig.GetSymbol(sig.GetAddress(k),buf,sizeof(buf));
    dbg << buf << "\n";
  }

  return dbg;
}

//////////////////////////////////////////////////////////////////////////////

DebugStackwalk::DebugStackwalk()
{
  // it doesn't harm to do this here
  InitDbghelp();
}

DebugStackwalk::~DebugStackwalk()
{
}

void *DebugStackwalk::GetDbghelpHandle()
{
  return g_dbghelp;
}

bool DebugStackwalk::IsOldDbghelp()
{
  return g_oldDbghelp;
}

int DebugStackwalk::StackWalk(Signature &sig, struct _CONTEXT *ctx)
{
  InitDbghelp();

  sig.m_numAddr=0;

  // bail out if no stack walk available
  if (!gDbg._StackWalk64)
    return 0;

	// Set up the stack frame structure for the start point of the stack walk (i.e. here).
	STACKFRAME64 stackFrame;
	memset(&stackFrame,0,sizeof(stackFrame));

	stackFrame.AddrPC.Mode = AddrModeFlat;
	stackFrame.AddrStack.Mode = AddrModeFlat;
	stackFrame.AddrFrame.Mode = AddrModeFlat;

	// Use the context struct if it was provided, else walk back from here.
	// StackWalk64 modifies the context, so walk a copy.
	CONTEXT walkCtx;
	if (ctx)
		walkCtx = *ctx;
	else
		RtlCaptureContext(&walkCtx);
	stackFrame.AddrPC.Offset = CTX_PC(walkCtx);
	stackFrame.AddrStack.Offset = CTX_STACK(walkCtx);
	stackFrame.AddrFrame.Offset = CTX_FRAME(walkCtx);

	// Walk the stack by the requested number of return address iterations.
  bool skipFirst=!ctx;
  while (sig.m_numAddr<Signature::MAX_ADDR&&
		     gDbg._StackWalk64(CTX_STACKWALK_MACHINE,GetCurrentProcess(),GetCurrentThread(),
                         &stackFrame,&walkCtx,nullptr,gDbg._SymFunctionTableAccess64,gDbg._SymGetModuleBase64,nullptr))
  {
    if (skipFirst)
      skipFirst=false;
    else
      sig.m_addr[sig.m_numAddr++]=(uintptr_t)stackFrame.AddrPC.Offset;
  }

	return sig.m_numAddr;
}
