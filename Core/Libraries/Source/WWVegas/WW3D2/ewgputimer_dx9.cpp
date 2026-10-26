/*
**	EastWind: Command & Conquer Generals Zero Hour engine fork
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

#include "ewgputimer_dx9.h"
#include "ewgraphicsoptions_dx9.h"
#include "dx9wrapper.h"

#include <string.h>

namespace
{
	// Query sets in flight. Results are ready two or three frames after they were issued.
	const int RING = 4;
	const int WINDOW = 60;

	struct QuerySet
	{
		IDirect3DQuery9 *Disjoint;
		IDirect3DQuery9 *Freq;
		IDirect3DQuery9 *Stamp[EwGpuTimer::POINT_COUNT];
		bool Issued[EwGpuTimer::POINT_COUNT];
		bool Open;		// begun, FRAME_END not reached yet
		bool Pending;	// closed, results not read yet
	};

	QuerySet Sets[RING];
	int Current = -1;
	bool Unsupported = false;

	// The window being summed, and the last finished one.
	double SumTotal;
	double SumPart[EwGpuTimer::POINT_COUNT];
	int Hits[EwGpuTimer::POINT_COUNT];
	int Frames;
	float LastTotal;
	float LastPart[EwGpuTimer::POINT_COUNT];
	int LastFrames;

	template <class T> void Release_Com(T *&p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	bool Create_Queries(IDirect3DDevice9 *device)
	{
		for (int s = 0; s < RING; ++s)
		{
			QuerySet &q = Sets[s];
			if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &q.Disjoint))
				|| FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &q.Freq)))
				return false;
			for (int p = 0; p < EwGpuTimer::POINT_COUNT; ++p)
			{
				if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.Stamp[p])))
					return false;
			}
		}
		return true;
	}

	// S_OK: read. S_FALSE: not ready yet. Anything else (device lost): the set is lost.
	HRESULT Read(QuerySet &q, BOOL *disjoint, UINT64 *freq, UINT64 stamp[EwGpuTimer::POINT_COUNT])
	{
		HRESULT hr = q.Disjoint->GetData(disjoint, sizeof(*disjoint), 0);
		if (hr == S_OK)
			hr = q.Freq->GetData(freq, sizeof(*freq), 0);
		for (int p = 0; p < EwGpuTimer::POINT_COUNT && hr == S_OK; ++p)
		{
			if (q.Issued[p])
				hr = q.Stamp[p]->GetData(&stamp[p], sizeof(stamp[p]), 0);
		}
		return hr;
	}

	void Add_Frame(const QuerySet &q, UINT64 freq, const UINT64 stamp[EwGpuTimer::POINT_COUNT])
	{
		const double ms = 1000.0 / (double)freq;
		UINT64 previous = stamp[EwGpuTimer::FRAME_BEGIN];
		for (int p = EwGpuTimer::FRAME_BEGIN + 1; p < EwGpuTimer::POINT_COUNT; ++p)
		{
			if (!q.Issued[p])
				continue;
			SumPart[p] += (double)(stamp[p] - previous) * ms;
			++Hits[p];
			previous = stamp[p];
		}
		SumTotal += (double)(stamp[EwGpuTimer::FRAME_END] - stamp[EwGpuTimer::FRAME_BEGIN]) * ms;
		if (++Frames < WINDOW)
			return;

		LastTotal = (float)(SumTotal / Frames);
		for (int p = 0; p < EwGpuTimer::POINT_COUNT; ++p)
			LastPart[p] = Hits[p] > 0 ? (float)(SumPart[p] / Hits[p]) : -1.0f;
		LastFrames = Frames;
		SumTotal = 0;
		memset(SumPart, 0, sizeof(SumPart));
		memset(Hits, 0, sizeof(Hits));
		Frames = 0;
	}

	void Harvest()
	{
		for (int s = 0; s < RING; ++s)
		{
			QuerySet &q = Sets[s];
			if (!q.Pending)
				continue;
			BOOL disjoint = TRUE;
			UINT64 freq = 0;
			UINT64 stamp[EwGpuTimer::POINT_COUNT];
			const HRESULT hr = Read(q, &disjoint, &freq, stamp);
			if (hr == S_FALSE)
				continue;
			q.Pending = false;
			if (hr == S_OK && !disjoint && freq != 0)
				Add_Frame(q, freq, stamp);
		}
	}
}

void EwGpuTimer::Mark(Point point)
{
	if (!EwGraphics().DebugKeys || Unsupported)
		return;
	IDirect3DDevice9 *device = DX8Wrapper::_Get_D3D_Device9();
	if (device == nullptr)
		return;

	if (point == FRAME_BEGIN)
	{
		if (Current < 0 && !Create_Queries(device))
		{
			Release();
			Unsupported = true;
			return;
		}
		Harvest();
		// A frame that never reached FRAME_END (render skipped) is dropped.
		if (Current >= 0 && Sets[Current].Open)
		{
			Sets[Current].Disjoint->Issue(D3DISSUE_END);
			Sets[Current].Open = false;
		}
		Current = (Current + 1) % RING;
		QuerySet &q = Sets[Current];
		q.Pending = false;	// still not read after a full ring: dropped
		memset(q.Issued, 0, sizeof(q.Issued));
		q.Disjoint->Issue(D3DISSUE_BEGIN);
		q.Open = true;
	}

	if (Current < 0 || !Sets[Current].Open || Sets[Current].Issued[point])
		return;
	QuerySet &q = Sets[Current];
	q.Stamp[point]->Issue(D3DISSUE_END);
	q.Issued[point] = true;
	if (point == FRAME_END)
	{
		q.Disjoint->Issue(D3DISSUE_END);
		q.Freq->Issue(D3DISSUE_END);
		q.Open = false;
		q.Pending = true;
	}
}

int EwGpuTimer::Average(float *total_ms, float part_ms[POINT_COUNT])
{
	if (Unsupported)
		return -1;
	*total_ms = LastTotal;
	memcpy(part_ms, LastPart, sizeof(LastPart));
	return LastFrames;
}

void EwGpuTimer::Release()
{
	for (int s = 0; s < RING; ++s)
	{
		QuerySet &q = Sets[s];
		Release_Com(q.Disjoint);
		Release_Com(q.Freq);
		for (int p = 0; p < POINT_COUNT; ++p)
			Release_Com(q.Stamp[p]);
		q.Open = false;
		q.Pending = false;
	}
	Current = -1;
	Unsupported = false;
}
