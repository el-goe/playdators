// Radix sort, originally based on https://github.com/bitshifter/radixsort
// however changed to treat only upper 16 bits of 32 bit integer as the keys,
// and keeps the lower 16 bits as "values". Removed all other code paths.

/*
 * Copyright (c) 2014 Cameron Hart
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */
#include "radixsort.h"
#include <string.h>

#define kRadixBits 8 // each radix pass is 8 bits at a time
#define kHistBuckets 2 // we only sort by upper 16 bits, so only two buckets are needed
#define kHistSize 256 // histogram size to hold 8 bits of values

/**
  * Initialise each histogram bucket with the key values
  */
static void init_histograms_u32(uint32_t* restrict hist, const uint32_t* restrict keys_in, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        const uint32_t key = keys_in[i];
        const uint32_t pos2 = (key >> 16) & 0xFF;
        const uint32_t pos3 = (key >> 24);
        hist[pos2]++;
        hist[pos3 + kHistSize]++;
    }
}

/**
 * Update the histogram data so each entry sums the previous entries.
 */
static void sum_histograms(uint32_t* restrict hist)
{
    uint32_t sum[kHistBuckets];
    for (uint32_t bucket = 0; bucket < kHistBuckets; ++bucket)
    {
        uint32_t* restrict offset = hist + (bucket * kHistSize);
        sum[bucket] = offset[0];
        offset[0] = 0;
    }

    uint32_t tsum;
    for (uint32_t i = 1; i < kHistSize; ++i)
    {
        for (uint32_t bucket = 0; bucket < kHistBuckets; ++bucket)
        {
            uint32_t* restrict offset = hist + (bucket * kHistSize);
            tsum = offset[i] + sum[bucket];
            offset[i] = sum[bucket];
            sum[bucket] = tsum;
        }
    }
}

static inline void radixpass_u32(uint32_t* restrict hist, uint32_t shift,
    const uint32_t* restrict keys_in, uint32_t* restrict keys_out, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i)
    {
        const uint32_t key = keys_in[i];
        const uint32_t pos = (key >> shift) & 0xFF;
        const uint32_t index = hist[pos]++;
        keys_out[index] = key;
    }
}

void radixsort_upper16keys(uint32_t* restrict keys_in_out, uint32_t* restrict keys_tmp, uint32_t size)
{
    uint32_t hist[kHistBuckets * kHistSize];
    memset(hist, 0, sizeof(hist));

    init_histograms_u32(hist, keys_in_out, size);

    sum_histograms(hist);

    // two radix passes
    radixpass_u32(hist, 16, keys_in_out, keys_tmp, size);
    radixpass_u32(hist + kHistSize, 24, keys_tmp, keys_in_out, size);
}
