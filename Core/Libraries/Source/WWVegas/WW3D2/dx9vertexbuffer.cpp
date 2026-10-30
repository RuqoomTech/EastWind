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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx9vertexbuffer.cpp                    $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/26/02 5:06p                                             $*
 *                                                                                             *
 *                    $Revision:: 39                                                          $*
 *                                                                                             *
 * 06/26/02 KM VB Vertex format size update for shaders                                       *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define VERTEX_BUFFER_LOG

#include "dx9vertexbuffer.h"
#include "dx9wrapper.h"
#include "dx9fvf.h"
#include "dx9caps.h"
#include "WWLib/thread.h"
#include "WWDebug/wwmemlog.h"

#define DEFAULT_VB_SIZE 5000

static bool _DynamicSortingVertexArrayInUse=false;
//static VertexFormatXYZNDUV2* _DynamicSortingVertexArray=nullptr;
static SortingVertexBufferClass* _DynamicSortingVertexArray=nullptr;
static unsigned short _DynamicSortingVertexArraySize=0;
static unsigned short _DynamicSortingVertexArrayOffset=0;

static bool _DynamicDX9VertexBufferInUse=false;
static DX9VertexBufferClass* _DynamicDX9VertexBuffer=nullptr;
static unsigned short _DynamicDX9VertexBufferSize=DEFAULT_VB_SIZE;
static unsigned short _DynamicDX9VertexBufferOffset=0;

static const FVFInfoClass _DynamicFVFInfo(dynamic_fvf_type);

static int _DX9VertexBufferCount=0;

static int _VertexBufferCount;
static int _VertexBufferTotalVertices;
static int _VertexBufferTotalSize;

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::VertexBufferClass(unsigned type_, unsigned FVF, unsigned short vertex_count_)
	:
	VertexCount(vertex_count_),
	type(type_),
	engine_refs(0)
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(VertexCount);
	WWASSERT(type==BUFFER_TYPE_DX9 || type==BUFFER_TYPE_SORTING);
	WWASSERT(FVF != 0);
	fvf_info=W3DNEW FVFInfoClass(FVF);

	_VertexBufferCount++;
	_VertexBufferTotalVertices+=VertexCount;
	_VertexBufferTotalSize+=VertexCount*fvf_info->Get_FVF_Size();
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("New VB, %d vertices, size %d bytes",VertexCount,VertexCount*fvf_info->Get_FVF_Size()));
	WWDEBUG_SAY(("Total VB count: %d, total %d vertices, total size %d bytes",
		_VertexBufferCount,
		_VertexBufferTotalVertices,
		_VertexBufferTotalSize));
#endif
}

// ----------------------------------------------------------------------------

VertexBufferClass::~VertexBufferClass()
{
	_VertexBufferCount--;
	_VertexBufferTotalVertices-=VertexCount;
	_VertexBufferTotalSize-=VertexCount*fvf_info->Get_FVF_Size();

#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("Delete VB, %d vertices, size %d bytes",VertexCount,VertexCount*fvf_info->Get_FVF_Size()));
	WWDEBUG_SAY(("Total VB count: %d, total %d vertices, total size %d bytes",
		_VertexBufferCount,
		_VertexBufferTotalVertices,
		_VertexBufferTotalSize));
#endif
	delete fvf_info;
}

unsigned VertexBufferClass::Get_Total_Buffer_Count()
{
	return _VertexBufferCount;
}

unsigned VertexBufferClass::Get_Total_Allocated_Vertices()
{
	return _VertexBufferTotalVertices;
}

unsigned VertexBufferClass::Get_Total_Allocated_Memory()
{
	return _VertexBufferTotalSize;
}


// ----------------------------------------------------------------------------

void VertexBufferClass::Add_Engine_Ref() const
{
	engine_refs++;
}

// ----------------------------------------------------------------------------

void VertexBufferClass::Release_Engine_Ref() const
{
	engine_refs--;
	WWASSERT(engine_refs>=0);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::WriteLockClass::WriteLockClass(VertexBufferClass* VertexBuffer, int flags)
	:
	VertexBufferLockClass(VertexBuffer)
{
	DX9_THREAD_ASSERT();
	WWASSERT(VertexBuffer);
	WWASSERT(!VertexBuffer->Engine_Refs());
	VertexBuffer->Add_Ref();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX9:
#ifdef VERTEX_BUFFER_LOG
		{
		StringClass fvf_name;
		VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("VertexBuffer->Lock(start_index: 0, index_range: 0(%d), fvf_size: %d, fvf: %s)",
			VertexBuffer->Get_Vertex_Count(),
			VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
		DX9_Assert();
		DX9_ErrorCode(static_cast<DX9VertexBufferClass*>(VertexBuffer)->Get_DX9_Vertex_Buffer()->Lock(
			0,
			0,
			(void**)&Vertices,
			flags));	//flags
		break;
	case BUFFER_TYPE_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(VertexBuffer)->VertexBuffer;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

VertexBufferClass::WriteLockClass::~WriteLockClass()
{
	DX9_THREAD_ASSERT();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX9:
#ifdef VERTEX_BUFFER_LOG
		WWDEBUG_SAY(("VertexBuffer->Unlock()"));
#endif
		DX9_Assert();
		DX9_ErrorCode(static_cast<DX9VertexBufferClass*>(VertexBuffer)->Get_DX9_Vertex_Buffer()->Unlock());
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	VertexBuffer->Release_Ref();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::AppendLockClass::AppendLockClass(VertexBufferClass* VertexBuffer,unsigned start_index, unsigned index_range)
	:
	VertexBufferLockClass(VertexBuffer)
{
	DX9_THREAD_ASSERT();
	WWASSERT(VertexBuffer);
	WWASSERT(!VertexBuffer->Engine_Refs());
	WWASSERT(start_index+index_range<=VertexBuffer->Get_Vertex_Count());
	VertexBuffer->Add_Ref();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX9:
#ifdef VERTEX_BUFFER_LOG
		{
		StringClass fvf_name;
		VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("VertexBuffer->Lock(start_index: %d, index_range: %d, fvf_size: %d, fvf: %s)",
			start_index,
			index_range,
			VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
		DX9_Assert();
		DX9_ErrorCode(static_cast<DX9VertexBufferClass*>(VertexBuffer)->Get_DX9_Vertex_Buffer()->Lock(
			start_index*VertexBuffer->FVF_Info().Get_FVF_Size(),
			index_range*VertexBuffer->FVF_Info().Get_FVF_Size(),
			(void**)&Vertices,
			0));	// Default (no) flags
		break;
	case BUFFER_TYPE_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(VertexBuffer)->VertexBuffer+start_index;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

VertexBufferClass::AppendLockClass::~AppendLockClass()
{
	DX9_THREAD_ASSERT();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX9:
		DX9_Assert();
#ifdef VERTEX_BUFFER_LOG
		WWDEBUG_SAY(("VertexBuffer->Unlock()"));
#endif
		DX9_ErrorCode(static_cast<DX9VertexBufferClass*>(VertexBuffer)->Get_DX9_Vertex_Buffer()->Unlock());
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	VertexBuffer->Release_Ref();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

SortingVertexBufferClass::SortingVertexBufferClass(unsigned short VertexCount)
	:
	VertexBufferClass(BUFFER_TYPE_SORTING, dynamic_fvf_type, VertexCount)
{
	WWMEMLOG(MEM_RENDERER);
	VertexBuffer=W3DNEWARRAY VertexFormatXYZNDUV2[VertexCount];
}

// ----------------------------------------------------------------------------

SortingVertexBufferClass::~SortingVertexBufferClass()
{
	delete[] VertexBuffer;
}


// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

//	bool dynamic=false,bool softwarevp=false);

DX9VertexBufferClass::DX9VertexBufferClass(unsigned FVF, unsigned short vertex_count_, UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX9, FVF, vertex_count_),
	VertexBuffer(nullptr)
{
	Create_Vertex_Buffer(usage);
}

// ----------------------------------------------------------------------------

DX9VertexBufferClass::DX9VertexBufferClass(
	const Vector3* vertices,
	const Vector3* normals,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX9, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_NORMAL, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(normals);
	WWASSERT(tex_coords);

	Create_Vertex_Buffer(usage);
	Copy(vertices,normals,tex_coords,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX9VertexBufferClass::DX9VertexBufferClass(
	const Vector3* vertices,
	const Vector3* normals,
	const Vector4* diffuse,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX9, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_NORMAL|D3DFVF_DIFFUSE, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(normals);
	WWASSERT(tex_coords);
	WWASSERT(diffuse);

	Create_Vertex_Buffer(usage);
	Copy(vertices,normals,tex_coords,diffuse,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX9VertexBufferClass::DX9VertexBufferClass(
	const Vector3* vertices,
	const Vector4* diffuse,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX9, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_DIFFUSE, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(tex_coords);
	WWASSERT(diffuse);

	Create_Vertex_Buffer(usage);
	Copy(vertices,tex_coords,diffuse,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX9VertexBufferClass::DX9VertexBufferClass(
	const Vector3* vertices,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX9, D3DFVF_XYZ|D3DFVF_TEX1, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(tex_coords);

	Create_Vertex_Buffer(usage);
	Copy(vertices,tex_coords,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX9VertexBufferClass::~DX9VertexBufferClass()
{
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("VertexBuffer->Release()"));
	_DX9VertexBufferCount--;
	WWDEBUG_SAY(("Current vertex buffer count: %d",_DX9VertexBufferCount));
#endif
	VertexBuffer->Release();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX9VertexBufferClass::Create_Vertex_Buffer(UsageType usage)
{
	DX9_THREAD_ASSERT();
	WWASSERT(!VertexBuffer);

#ifdef VERTEX_BUFFER_LOG
	StringClass fvf_name;
	FVF_Info().Get_FVF_Name(fvf_name);
	WWDEBUG_SAY(("CreateVertexBuffer(fvfsize=%d, vertex_count=%d, D3DUSAGE_WRITEONLY|%s|%s, fvf: %s, %s)",
		FVF_Info().Get_FVF_Size(),
		VertexCount,
		(usage&USAGE_DYNAMIC) ? "D3DUSAGE_DYNAMIC" : "-",
		(usage&USAGE_SOFTWAREPROCESSING) ? "D3DUSAGE_SOFTWAREPROCESSING" : "-",
		fvf_name,
		"D3DPOOL_DEFAULT"));
	_DX9VertexBufferCount++;
	WWDEBUG_SAY(("Current vertex buffer count: %d",_DX9VertexBufferCount));
#endif

	unsigned usage_flags=
		D3DUSAGE_WRITEONLY|
		((usage&USAGE_DYNAMIC) ? D3DUSAGE_DYNAMIC : 0)|
		((usage&USAGE_NPATCHES) ? D3DUSAGE_NPATCHES : 0)|
		((usage&USAGE_SOFTWAREPROCESSING) ? D3DUSAGE_SOFTWAREPROCESSING : 0);
	// New Code
	if (!DX9Wrapper::Get_Current_Caps()->Support_TnL()) {
		usage_flags|=D3DUSAGE_SOFTWAREPROCESSING;
	}

	HRESULT ret=DX9Wrapper::_Get_D3D_Device9()->CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		D3DPOOL_DEFAULT,	// D3D9Ex has no managed pool; default-pool buffers survive a device reset.
		&VertexBuffer,
		nullptr);
	if (SUCCEEDED(ret)) {
		return;
	}

	WWDEBUG_SAY(("Vertex buffer creation failed, trying to release assets..."));

	// Vertex buffer creation failed, so try releasing least used textures and flushing the mesh cache.

	// Free all textures that haven't been used in the last 5 seconds
	TextureClass::Invalidate_Old_Unused_Textures(5000);

	// Invalidate the mesh cache
	WW3D::_Invalidate_Mesh_Cache();

	//@todo: Find some way to invalidate the textures too
	ret = DX9Wrapper::_Get_D3D_Device9()->EvictManagedResources();

	// Try again...
	ret=DX9Wrapper::_Get_D3D_Device9()->CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		D3DPOOL_DEFAULT,	// D3D9Ex has no managed pool; default-pool buffers survive a device reset.
		&VertexBuffer,
		nullptr);

	if (SUCCEEDED(ret)) {
		WWDEBUG_SAY(("...Vertex buffer creation successful"));
	}

	// If it still fails it is fatal
	DX9_ErrorCode(ret);

	/* Old Code
	DX9CALL(CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&VertexBuffer));
	*/
}

// ----------------------------------------------------------------------------

void DX9VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, unsigned first_vertex,unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(uv);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX9_FVF_XYZNUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZNUV1* verts=(VertexFormatXYZNUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZNUV1* verts=(VertexFormatXYZNUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
}

// ----------------------------------------------------------------------------

void DX9VertexBufferClass::Copy(const Vector3* loc, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX9_FVF_XYZ);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZ* verts=(VertexFormatXYZ*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZ* verts=(VertexFormatXYZ*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
		}
	}

}

// ----------------------------------------------------------------------------

void DX9VertexBufferClass::Copy(const Vector3* loc, const Vector2* uv, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(uv);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX9_FVF_XYZUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZUV1* verts=(VertexFormatXYZUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZUV1* verts=(VertexFormatXYZUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
}

// ----------------------------------------------------------------------------

void DX9VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX9_FVF_XYZN);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZN* verts=(VertexFormatXYZN*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZN* verts=(VertexFormatXYZN*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
		}
	}
}

// ----------------------------------------------------------------------------

void DX9VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(uv);
	WWASSERT(diffuse);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX9_FVF_XYZNDUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZNDUV1* verts=(VertexFormatXYZNDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX9Wrapper::Convert_Color(diffuse[v]);
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZNDUV1* verts=(VertexFormatXYZNDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX9Wrapper::Convert_Color(diffuse[v]);
		}
	}
}

// ----------------------------------------------------------------------------

void DX9VertexBufferClass::Copy(const Vector3* loc, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(uv);
	WWASSERT(diffuse);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX9_FVF_XYZDUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZDUV1* verts=(VertexFormatXYZDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX9Wrapper::Convert_Color(diffuse[v]);
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZDUV1* verts=(VertexFormatXYZDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX9Wrapper::Convert_Color(diffuse[v]);
		}
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DynamicVBAccessClass::DynamicVBAccessClass(unsigned t,unsigned fvf,unsigned short vertex_count_)
	:
	Type(t),
	FVFInfo(_DynamicFVFInfo),
	VertexCount(vertex_count_),
	VertexBuffer(nullptr)
{
	WWASSERT(fvf==dynamic_fvf_type);
	WWASSERT(Type==BUFFER_TYPE_DYNAMIC_DX9 || Type==BUFFER_TYPE_DYNAMIC_SORTING);

	if (Type==BUFFER_TYPE_DYNAMIC_DX9) {
		Allocate_DX9_Dynamic_Buffer();
	}
	else {
		Allocate_Sorting_Dynamic_Buffer();
	}
}

DynamicVBAccessClass::~DynamicVBAccessClass()
{
	if (Type==BUFFER_TYPE_DYNAMIC_DX9) {
		_DynamicDX9VertexBufferInUse=false;
		_DynamicDX9VertexBufferOffset+=(unsigned) VertexCount;
	}
	else {
		_DynamicSortingVertexArrayInUse=false;
		_DynamicSortingVertexArrayOffset+=VertexCount;
	}

	REF_PTR_RELEASE (VertexBuffer);
}

// ----------------------------------------------------------------------------

void DynamicVBAccessClass::_Deinit()
{
	WWASSERT ((_DynamicDX9VertexBuffer == nullptr) || (_DynamicDX9VertexBuffer->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicDX9VertexBuffer);
	_DynamicDX9VertexBufferInUse=false;
	_DynamicDX9VertexBufferSize=DEFAULT_VB_SIZE;
	_DynamicDX9VertexBufferOffset=0;

	WWASSERT ((_DynamicSortingVertexArray == nullptr) || (_DynamicSortingVertexArray->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicSortingVertexArray);
	WWASSERT(!_DynamicSortingVertexArrayInUse);
	_DynamicSortingVertexArrayInUse=false;
	_DynamicSortingVertexArraySize=0;
	_DynamicSortingVertexArrayOffset=0;
}

void DynamicVBAccessClass::Allocate_DX9_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicDX9VertexBufferInUse);
	_DynamicDX9VertexBufferInUse=true;

	// If requesting more vertices than dynamic vertex buffer can fit, delete the vb
	// and adjust the size to the new count.
	if (VertexCount>_DynamicDX9VertexBufferSize) {
		REF_PTR_RELEASE(_DynamicDX9VertexBuffer);
		_DynamicDX9VertexBufferSize=VertexCount;
		if (_DynamicDX9VertexBufferSize<DEFAULT_VB_SIZE) _DynamicDX9VertexBufferSize=DEFAULT_VB_SIZE;
	}

	// Create a new vb if one doesn't exist currently
	if (!_DynamicDX9VertexBuffer) {
		unsigned usage=DX9VertexBufferClass::USAGE_DYNAMIC;
		if (DX9Wrapper::Get_Current_Caps()->Support_NPatches()) {
			usage|=DX9VertexBufferClass::USAGE_NPATCHES;
		}

		_DynamicDX9VertexBuffer=NEW_REF(DX9VertexBufferClass,(
			dynamic_fvf_type,
			_DynamicDX9VertexBufferSize,
			(DX9VertexBufferClass::UsageType)usage));
		_DynamicDX9VertexBufferOffset=0;
	}

	// Any room at the end of the buffer?
	if (((unsigned)VertexCount+_DynamicDX9VertexBufferOffset)>_DynamicDX9VertexBufferSize) {
		_DynamicDX9VertexBufferOffset=0;
	}

	REF_PTR_SET(VertexBuffer,_DynamicDX9VertexBuffer);
	VertexBufferOffset=_DynamicDX9VertexBufferOffset;
}

void DynamicVBAccessClass::Allocate_Sorting_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicSortingVertexArrayInUse);
	_DynamicSortingVertexArrayInUse=true;

	unsigned new_vertex_count=_DynamicSortingVertexArrayOffset+VertexCount;
	WWASSERT(new_vertex_count<65536);
	if (new_vertex_count>_DynamicSortingVertexArraySize) {
		REF_PTR_RELEASE(_DynamicSortingVertexArray);
		_DynamicSortingVertexArraySize=new_vertex_count;
		if (_DynamicSortingVertexArraySize<DEFAULT_VB_SIZE) _DynamicSortingVertexArraySize=DEFAULT_VB_SIZE;
	}

	if (!_DynamicSortingVertexArray) {
		_DynamicSortingVertexArray=NEW_REF(SortingVertexBufferClass,(_DynamicSortingVertexArraySize));
		_DynamicSortingVertexArrayOffset=0;
	}

	REF_PTR_SET(VertexBuffer,_DynamicSortingVertexArray);
	VertexBufferOffset=_DynamicSortingVertexArrayOffset;
}

// ----------------------------------------------------------------------------
static int dx9_lock;
DynamicVBAccessClass::WriteLockClass::WriteLockClass(DynamicVBAccessClass* dynamic_vb_access_)
	:
	DynamicVBAccess(dynamic_vb_access_)
{
	DX9_THREAD_ASSERT();
	switch (DynamicVBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX9:
#ifdef VERTEX_BUFFER_LOG
		{
		WWASSERT(!dx9_lock);
		dx9_lock++;
		StringClass fvf_name;
		DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("DynamicVertexBuffer->Lock(start_index: %d, index_range: %d, fvf_size: %d, fvf: %s)",
			DynamicVBAccess->VertexBufferOffset,
			DynamicVBAccess->Get_Vertex_Count(),
			DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
		WWASSERT(_DynamicDX9VertexBuffer);
//		WWASSERT(!_DynamicDX9VertexBuffer->Engine_Refs());

		DX9_Assert();
		// Lock with discard contents if the buffer offset is zero
		DX9_ErrorCode(static_cast<DX9VertexBufferClass*>(DynamicVBAccess->VertexBuffer)->Get_DX9_Vertex_Buffer()->Lock(
			DynamicVBAccess->VertexBufferOffset*_DynamicDX9VertexBuffer->FVF_Info().Get_FVF_Size(),
			DynamicVBAccess->Get_Vertex_Count()*DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
			(void**)&Vertices,
			D3DLOCK_NOSYSLOCK | (!DynamicVBAccess->VertexBufferOffset ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE)));
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(DynamicVBAccess->VertexBuffer)->VertexBuffer;
		Vertices+=DynamicVBAccess->VertexBufferOffset;
//		vertices=_DynamicSortingVertexArray+_DynamicSortingVertexArrayOffset;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

DynamicVBAccessClass::WriteLockClass::~WriteLockClass()
{
	DX9_THREAD_ASSERT();
	switch (DynamicVBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX9:
#ifdef VERTEX_BUFFER_LOG
		dx9_lock--;
		WWASSERT(!dx9_lock);
		WWDEBUG_SAY(("DynamicVertexBuffer->Unlock()"));
#endif
		DX9_Assert();
		DX9_ErrorCode(static_cast<DX9VertexBufferClass*>(DynamicVBAccess->VertexBuffer)->Get_DX9_Vertex_Buffer()->Unlock());
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

void DynamicVBAccessClass::_Reset(bool frame_changed)
{
	_DynamicSortingVertexArrayOffset=0;
	if (frame_changed) _DynamicDX9VertexBufferOffset=0;
}

unsigned short DynamicVBAccessClass::Get_Default_Vertex_Count()
{
	return _DynamicDX9VertexBufferSize;
}

