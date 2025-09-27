#include <assert.h>
#include <string.h>
#include "adjacency_generator.h"
#include "../platform.h"

// Based on meshopt_generateAdjacencyIndexBuffer from meshoptimizer project (MIT license):
// https://github.com/zeux/meshoptimizer
// Copyright (c) 2016-2025, Arseny Kapoulkine

// "edge" is two vertex indices in each 16 bit part
static uint32_t edge_hash(uint32_t edge)
{
	return edge * 0xdeece66d + 0xb;
}

static uint32_t edge_hash_buckets(uint32_t count)
{
	uint32_t buckets = 1;
	while (buckets < count + count / 4)
		buckets *= 2;
	return buckets;
}

static uint32_t* edge_hash_lookup(uint32_t* table, uint32_t buckets, uint32_t key, uint32_t empty)
{
	assert(buckets > 0);
	assert((buckets & (buckets - 1)) == 0);

	uint32_t hashmod = buckets - 1;
	uint32_t bucket = edge_hash(key) & hashmod;

	for (uint32_t probe = 0; probe <= hashmod; ++probe)
	{
		uint32_t* item = &table[bucket];
		if (*item == empty || *item == key)
			return item;

		// hash collision, quadratic probing
		bucket = (bucket + probe + 1) & hashmod;
	}

	assert(false && "Edge hash table is full"); // unreachable
	return NULL;
}

void generate_adjacency_buffer(uint16_t* destination, const uint16_t* indices, uint32_t index_count)
{
	assert(index_count % 3 == 0);

	// build edge set; this stores all triangle edges but we can look these up by any other wedge
	uint32_t edge_table_size = edge_hash_buckets(index_count);
	uint32_t* edge_table = (uint32_t*)plat_malloc(edge_table_size * sizeof(uint32_t));
	uint16_t* edge_face_table = (uint16_t*)plat_malloc(edge_table_size * sizeof(uint16_t));

	memset(edge_table, -1, edge_table_size * sizeof(uint32_t));
	memset(edge_face_table, -1, edge_table_size * sizeof(uint16_t));

	static const int next[4] = { 1, 2, 0, 1 };
	uint16_t fi = 0;
	for (uint32_t i = 0; i < index_count; i += 3, fi++)
	{
		for (int e = 0; e < 3; ++e)
		{
			uint16_t i0 = indices[i + e];
			uint16_t i1 = indices[i + next[e]];

			uint32_t edge = ((uint32_t)i0 << 16) | i1;
			uint32_t* entry = edge_hash_lookup(edge_table, edge_table_size, edge, ~0u);
			if (*entry == ~0u)
			{
				*entry = edge;
				// store face index
				edge_face_table[entry - edge_table] = fi;
			}
		}
	}

	// build result adjacency buffer: 3 adjacent face indices for each input triangle
	for (uint32_t i = 0; i < index_count; i += 3)
	{
		for (int e = 0; e < 3; ++e)
		{
			uint16_t i0 = indices[i + e];
			uint16_t i1 = indices[i + next[e]];

			// note: this refers to the opposite edge!
			uint32_t edge = ((uint32_t)i1 << 16) | i0;
			uint32_t* oppe = edge_hash_lookup(edge_table, edge_table_size, edge, ~0u);
			uint16_t res = (*oppe == ~0u) ? 0xFFFF : edge_face_table[oppe - edge_table];
			assert(res == 0xFFFF || res < index_count/3);
			destination[i + e] = res;
		}
	}

	plat_free(edge_table);
	plat_free(edge_face_table);
}
