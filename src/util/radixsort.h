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
#ifndef BITS_RADIXSORT_H
#define BITS_RADIXSORT_H

#include <stdint.h>
#include <stdlib.h>

#if _MSC_VER || __cplusplus
#define restrict __restrict
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Radix sorts 32 bit values; where upper 16 bits
// are the sorting key. Keeps lower 16 bit values intact, i.e. they
// are the "payload".
// 
// Both the input and the output are the same array. keys_temp
// is just for temporary storage during the radix passes; and should be the same size
// as input.
extern void radixsort_upper16keys(uint32_t* restrict keys_in_out, uint32_t* restrict keys_tmp, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif // BITS_RADIXSORT_H
