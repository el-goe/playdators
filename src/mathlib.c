// SPDX-License-Identifier: Unlicense

#include "mathlib.h"

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
	float t = 1.0f - c;
	axis = v3_normalize(axis);
	float x = axis.x;
	float y = axis.y;
	float z = axis.z;
	p.ax.x = c + x * x * t;
	p.ax.y = x * y * t + z * s;
	p.ax.z = x * z * t - y * s;

	p.ay.x = y * x * t - z * s;
	p.ay.y = c + y * y * t;
	p.ay.z = y * z * t + x * s;

	p.az.x = z * x * t + y * s;
	p.az.y = z * y * t - x * s;
	p.az.z = c + z * z * t;
	p.pos = f3(0, 0, 0);
	return p;
}

xform xform_multiply(const xform* l, const xform* r)
{
	xform m;
	m.ax.x = l->ax.x * r->ax.x + l->ay.x * r->ax.y + l->az.x * r->ax.z;
	m.ax.y = l->ax.y * r->ax.x + l->ay.y * r->ax.y + l->az.y * r->ax.z;
	m.ax.z = l->ax.z * r->ax.x + l->ay.z * r->ax.y + l->az.z * r->ax.z;
	m.ay.x = l->ax.x * r->ay.x + l->ay.x * r->ay.y + l->az.x * r->ay.z;
	m.ay.y = l->ax.y * r->ay.x + l->ay.y * r->ay.y + l->az.y * r->ay.z;
	m.ay.z = l->ax.z * r->ay.x + l->ay.z * r->ay.y + l->az.z * r->ay.z;
	m.az.x = l->ax.x * r->az.x + l->ay.x * r->az.y + l->az.x * r->az.z;
	m.az.y = l->ax.y * r->az.x + l->ay.y * r->az.y + l->az.y * r->az.z;
	m.az.z = l->ax.z * r->az.x + l->ay.z * r->az.y + l->az.z * r->az.z;
	m.pos.x = l->pos.x + l->ax.x * r->pos.x + l->ay.x * r->pos.y + l->az.x * r->pos.z;
	m.pos.y = l->pos.y + l->ax.y * r->pos.x + l->ay.y * r->pos.y + l->az.y * r->pos.z;
	m.pos.z = l->pos.z + l->ax.z * r->pos.x + l->ay.z * r->pos.y + l->az.z * r->pos.z;
	return m;
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
	if (v3_lensq(fw) < 1.0e-6f)
		fw = f3(0, 0, 1);
	else
		fw = v3_normalize(fw);
	// calculate right vector
	float3 rt = v3_cross(up, fw);
	// up is nearly parallel to forward, pick a different "up"
	if (v3_lensq(rt) < 1.0e-6f) {
		up = fabsf(fw.y) < 0.99f ? f3(0, 1, 0) : f3(1, 0, 0);
		rt = v3_cross(fw, up);
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
	r.ax.x = m->ax.x; r.ay.x = m->ax.y; r.az.x = m->ax.z;
	r.ax.y = m->ay.x; r.ay.y = m->ay.y; r.az.y = m->ay.z;
	r.ax.z = m->az.x; r.ay.z = m->az.y; r.az.z = m->az.z;
	r.pos.x = -v3_dot(m->ax, m->pos);
	r.pos.y = -v3_dot(m->ay, m->pos);
	r.pos.z = -v3_dot(m->az, m->pos);
	return r;
}
