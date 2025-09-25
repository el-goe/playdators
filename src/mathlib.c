// SPDX-License-Identifier: Unlicense

#include "mathlib.h"

float3 v3_tri_normal(const float3* p1, const float3* p2, const float3* p3)
{
	float3 v = v3_cross(
		(float3) { p2->x - p1->x, p2->y - p1->y, p2->z - p1->z },
		(float3) { p3->x - p1->x, p3->y - p1->y, p3->z - p1->z });
	return v3_normalize(v);
}

xform xform_identity = { .ax = {1, 0, 0}, .ay = {0, 1, 0}, .az = {0, 0, 1}, .pos = {0, 0, 0} };

xform xform_make(float xx, float xy, float xz, float yx, float yy, float yz, float zx, float zy, float zz)
{
	return (xform) { .ax = { xx, xy, xz }, .ay = { yx, yy, yz }, .az = { zx, zy, zz }, .pos = { 0, 0, 0 } };
}

xform xform_axes(float3 x, float3 y, float3 z, float3 pos)
{
	return (xform) { .ax = x, .ay = y, .az = z, .pos = pos };
}

xform xform_make_translate(float dx, float dy, float dz)
{
	return (xform) { .ax = { 1, 0, 0 }, .ay = { 0, 1, 0 }, .az = { 0, 0, 1 }, .pos = { dx, dy, dz } };
}

xform xform_make_axis_angle(float angle, float3 axis)
{
	xform p;
	float c = cosf(angle);
	float s = sinf(angle);
	float x = axis.x;
	float y = axis.y;
	float z = axis.z;
	float d = sqrtf(x * x + y * y + z * z);
	x /= d;
	y /= d;
	z /= d;
	p.ax.x = c + x * x * (1 - c);
	p.ax.y = x * y * (1 - c) - z * s;
	p.ax.z = x * z * (1 - c) + y * s;
	p.ay.x = y * x * (1 - c) + z * s;
	p.ay.y = c + y * y * (1 - c);
	p.ay.z = y * z * (1 - c) - x * s;
	p.az.x = z * x * (1 - c) - y * s;
	p.az.y = z * y * (1 - c) + x * s;
	p.az.z = c + z * z * (1 - c);
	p.pos = f3(0, 0, 0);
	return p;
}

xform xform_multiply(const xform* l, const xform* r)
{
	xform m;
	m.ax.x = l->ax.x * r->ax.x + l->ay.x * r->ax.y + l->az.x * r->ax.z;
	m.ay.x = l->ax.x * r->ay.x + l->ay.x * r->ay.y + l->az.x * r->ay.z;
	m.az.x = l->ax.x * r->az.x + l->ay.x * r->az.y + l->az.x * r->az.z;
	m.ax.y = l->ax.y * r->ax.x + l->ay.y * r->ax.y + l->az.y * r->ax.z;
	m.ay.y = l->ax.y * r->ay.x + l->ay.y * r->ay.y + l->az.y * r->ay.z;
	m.az.y = l->ax.y * r->az.x + l->ay.y * r->az.y + l->az.y * r->az.z;
	m.ax.z = l->ax.z * r->ax.x + l->ay.z * r->ax.y + l->az.z * r->ax.z;
	m.ay.z = l->ax.z * r->ay.x + l->ay.z * r->ay.y + l->az.z * r->ay.z;
	m.az.z = l->ax.z * r->az.x + l->ay.z * r->az.y + l->az.z * r->az.z;
	m.pos.x = l->pos.x * r->ax.x + l->pos.y * r->ax.y + l->pos.z * r->ax.z + r->pos.x;
	m.pos.y = l->pos.x * r->ay.x + l->pos.y * r->ay.y + l->pos.z * r->ay.z + r->pos.y;
	m.pos.z = l->pos.x * r->az.x + l->pos.y * r->az.y + l->pos.z * r->az.z + r->pos.z;
	return m;
}

float3 xform_transform_pt(const xform* m, float3 p)
{
	float x = p.x * m->ax.x + p.y * m->ax.y + p.z * m->ax.z + m->pos.x;
	float y = p.x * m->ay.x + p.y * m->ay.y + p.z * m->ay.z + m->pos.y;
	float z = p.x * m->az.x + p.y * m->az.y + p.z * m->az.z + m->pos.z;

	return (float3) { x, y, z };
}

float xform_get_determinant(xform* m)
{
	return m->ax.x * m->ay.y * m->az.z
		+ m->ax.y * m->ay.z * m->az.x
		+ m->ax.z * m->ay.x * m->az.y
		- m->az.x * m->ay.y * m->ax.z
		- m->ay.x * m->ax.y * m->az.z
		- m->ax.x * m->az.y * m->ay.z;
}

xform xform_look_at(float3 pos, float3 target, float3 up)
{
	// calculate forward vector
	float3 fw = v3_sub(target, pos);
	if (v3_lensq(&fw) < 1.0e-6f)
		fw = f3(1, 0, 0);
	else
		fw = v3_normalize(fw);
	// calculate right vector
	float3 rt = v3_cross(up, fw);
	// up is nearly parallel to forward, pick a different "up"
	if (v3_lensq(&rt) < 1.0e-6f) {
		up = fabsf(fw.y) < 0.99f ? f3(0, 1, 0) : f3(1, 0, 0);
		rt = v3_cross(up, fw);
	}
	rt = v3_normalize(rt);
	// calculate final up
	up = v3_cross(fw, rt);

	xform x;
	x.ax = rt;
	x.ay = up;
	x.az = fw;
	x.pos = pos;
	return x;
}

xform xform_invert(const xform* m)
{
	xform r;
	r.ax.x = m->ax.x; r.ay.x = m->ay.x; r.az.x = m->az.x;
	r.ax.y = m->ax.y; r.ay.y = m->ay.y; r.az.y = m->az.y;
	r.ax.z = m->ax.z; r.ay.z = m->ay.z; r.az.z = m->az.z;
	r.pos.x = -v3_dot(r.ax, m->pos);
	r.pos.y = -v3_dot(r.ay, m->pos);
	r.pos.z = -v3_dot(r.az, m->pos);
	return r;
}
