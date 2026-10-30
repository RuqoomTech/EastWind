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

// The few D3DX math helpers the DX8 code used, for the DX9 renderer, which does not use D3DX.
//
// Same conventions as D3DX: row-major storage laid out like D3DMATRIX, row vectors
// (v' = v * M), and the same argument order, so a call such as
// D3DXMatrixInverse(&out, nullptr, &m) becomes D3DMatrixInverse(&out, nullptr, &m).
//
// Sums are evaluated left to right, ((x*m00 + y*m10) + z*m20) + w*m30, the same order
// BezierSegment.h uses with USE_DETERMINISTIC_MATH, so game logic gets the same results.

#pragma once

#include <d3d9types.h>
#include <math.h>
#include <string.h>

struct D3DVector3
{
	float x, y, z;

	D3DVector3() {}
	D3DVector3(float fx, float fy, float fz) : x(fx), y(fy), z(fz) {}

	operator float*() { return &x; }
	operator const float*() const { return &x; }
};

struct D3DVector4
{
	float x, y, z, w;

	D3DVector4() {}
	D3DVector4(float fx, float fy, float fz, float fw) : x(fx), y(fy), z(fz), w(fw) {}

	// Lets a temporary be passed straight to Set*ShaderConstantF, as with D3DXVECTOR4.
	operator float*() { return &x; }
	operator const float*() const { return &x; }
};

struct D3DMatrix : public D3DMATRIX
{
	D3DMatrix() {}
	D3DMatrix(const D3DMATRIX& m) : D3DMATRIX(m) {}
	D3DMatrix(
		float f11, float f12, float f13, float f14,
		float f21, float f22, float f23, float f24,
		float f31, float f32, float f33, float f34,
		float f41, float f42, float f43, float f44)
	{
		_11 = f11; _12 = f12; _13 = f13; _14 = f14;
		_21 = f21; _22 = f22; _23 = f23; _24 = f24;
		_31 = f31; _32 = f32; _33 = f33; _34 = f34;
		_41 = f41; _42 = f42; _43 = f43; _44 = f44;
	}

	float& operator()(unsigned row, unsigned col) { return m[row][col]; }
	float operator()(unsigned row, unsigned col) const { return m[row][col]; }

	operator float*() { return &_11; }
	operator const float*() const { return &_11; }

	D3DMatrix operator*(const D3DMATRIX& b) const;
	D3DMatrix& operator*=(const D3DMATRIX& b) { *this = *this * b; return *this; }
};

const float D3D_PI = 3.141592654f;

inline D3DMatrix* D3DMatrixIdentity(D3DMatrix* out)
{
	*out = D3DMatrix(
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);
	return out;
}

inline D3DMatrix* D3DMatrixMultiply(D3DMatrix* out, const D3DMATRIX* a, const D3DMATRIX* b)
{
	// out may alias a or b.
	D3DMATRIX r;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			r.m[i][j] = ((a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j]) + a->m[i][2] * b->m[2][j]) + a->m[i][3] * b->m[3][j];
		}
	}
	memcpy(out, &r, sizeof(r));
	return out;
}

inline D3DMatrix D3DMatrix::operator*(const D3DMATRIX& b) const
{
	D3DMatrix r;
	D3DMatrixMultiply(&r, this, &b);
	return r;
}

inline D3DMatrix* D3DMatrixTranspose(D3DMatrix* out, const D3DMATRIX* m)
{
	D3DMATRIX r;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			r.m[i][j] = m->m[j][i];
		}
	}
	memcpy(out, &r, sizeof(r));
	return out;
}

inline D3DMatrix* D3DMatrixScaling(D3DMatrix* out, float sx, float sy, float sz)
{
	D3DMatrixIdentity(out);
	out->_11 = sx;
	out->_22 = sy;
	out->_33 = sz;
	return out;
}

inline D3DMatrix* D3DMatrixTranslation(D3DMatrix* out, float x, float y, float z)
{
	D3DMatrixIdentity(out);
	out->_41 = x;
	out->_42 = y;
	out->_43 = z;
	return out;
}

inline D3DMatrix* D3DMatrixRotationZ(D3DMatrix* out, float angle)
{
	const float s = sinf(angle);
	const float c = cosf(angle);
	D3DMatrixIdentity(out);
	out->_11 = c;
	out->_12 = s;
	out->_21 = -s;
	out->_22 = c;
	return out;
}

// General 4x4 inverse by cofactors. Returns nullptr when the matrix is singular, as D3DX does.
inline D3DMatrix* D3DMatrixInverse(D3DMatrix* out, float* determinant, const D3DMATRIX* src)
{
	const float* m = &src->_11;
	float inv[16];

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

	const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if (determinant) {
		*determinant = det;
	}
	if (det == 0.0f) {
		return nullptr;
	}
	const float inv_det = 1.0f / det;
	float* o = &out->_11;
	for (int i = 0; i < 16; ++i) {
		o[i] = inv[i] * inv_det;
	}
	return out;
}

inline D3DVector4* D3DVec4Transform(D3DVector4* out, const D3DVector4* v, const D3DMATRIX* m)
{
	const float x = v->x, y = v->y, z = v->z, w = v->w;
	out->x = ((x * m->m[0][0] + y * m->m[1][0]) + z * m->m[2][0]) + w * m->m[3][0];
	out->y = ((x * m->m[0][1] + y * m->m[1][1]) + z * m->m[2][1]) + w * m->m[3][1];
	out->z = ((x * m->m[0][2] + y * m->m[1][2]) + z * m->m[2][2]) + w * m->m[3][2];
	out->w = ((x * m->m[0][3] + y * m->m[1][3]) + z * m->m[2][3]) + w * m->m[3][3];
	return out;
}

// Transforms (x, y, z, 1).
inline D3DVector4* D3DVec3Transform(D3DVector4* out, const D3DVector3* v, const D3DMATRIX* m)
{
	const D3DVector4 v4(v->x, v->y, v->z, 1.0f);
	return D3DVec4Transform(out, &v4, m);
}

inline float D3DVec4Dot(const D3DVector4* a, const D3DVector4* b)
{
	return ((a->x * b->x + a->y * b->y) + a->z * b->z) + a->w * b->w;
}
