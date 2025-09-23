#pragma once

#include "mathlib.h"

void draw_tri_pattern(uint8_t* bitmap, int rowstride, const float3* p0, const float3* p1, const float3* p2, const uint8_t* pattern);
void draw_tri_bluenoise(uint8_t* bitmap, int rowstride, const float3* p0, const float3* p1, const float3* p2, const uint8_t tri_color);

void draw_tri_checker(uint8_t* bitmap, int rowstride, const float3* p0, const float3* p1, const float3* p2, const float uvs[6]);
