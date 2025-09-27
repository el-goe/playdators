#pragma once

#include <stdint.h>

// For each triangle in the input triangle buffer, outputs 3 neighboring triangle indices
// (or 0xFFFF if there's no adjacent triangle).
void generate_adjacency_buffer(uint16_t* destination, const uint16_t* indices, uint32_t index_count);
