////////////////////////////////////////////////////////////////////////////////
// Copyright 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License.  You may obtain a copy
// of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
// License for the specific language governing permissions and limitations
// under the License.
////////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Common shared include file to hide compiler/os specific functions from the rest of the code. 
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(_MSC_VER) && !defined(__INTEL_COMPILER) && !defined(__clang__)
	#define __MICROSOFT_COMPILER
#endif

#if defined(_WIN32)	&& (defined(_MSC_VER) || defined(__INTEL_COMPILER) || defined(__clang__)) // Windows: MSVC / Intel compiler / clang
	#include <intrin.h>
	#include <new.h>

	#define FORCE_INLINE __forceinline

	FORCE_INLINE unsigned long find_clear_lsb(unsigned int *mask)
	{
		unsigned long idx;
		_BitScanForward(&idx, *mask);
		*mask &= *mask - 1;
		return idx;
	}

	FORCE_INLINE void *aligned_alloc(size_t alignment, size_t size)
	{
		return _aligned_malloc(size, alignment);
	}

	FORCE_INLINE void aligned_free(void *ptr)
	{
		_aligned_free(ptr);
	}

#elif defined(__GNUG__)	|| defined(__clang__) // G++ or clang

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__EMSCRIPTEN__)
#if defined(__EMSCRIPTEN__)
	// WebAssembly: Emscripten maps SSE..SSE4.1 intrinsics onto wasm SIMD128
	// (requires -msimd128 -msse4.1). There is no CPUID, so dispatch is fixed to SSE4.1.
	#include <smmintrin.h>
	// wasm has no MXCSR: float->int conversion always rounds to nearest-even, which is the only
	// mode the SSE4.1 rasterizer asks for (PRECISE_COVERAGE). The SSE2 fallback's floor/ceil,
	// the only code wanting another mode, use real floor/ceil on wasm (MaskedOcclusionCulling.cpp).
	#ifndef _MM_SET_ROUNDING_MODE
		#define _MM_SET_ROUNDING_MODE(mode) ((void)(mode))
	#endif
#else
	// ARM/NEON: use sse2neon to translate SSE intrinsics to NEON
	#include "sse2neon.h"
#endif
	#include <stdlib.h>
	#include <new>

	#define FORCE_INLINE inline

	FORCE_INLINE unsigned long find_clear_lsb(unsigned int *mask)
	{
		unsigned long idx;
		idx = __builtin_ctzl(*mask);
		*mask &= *mask - 1;
		return idx;
	}

	FORCE_INLINE void *aligned_alloc(size_t alignment, size_t size)
	{
		void *ptr = nullptr;
		posix_memalign(&ptr, alignment, size);
		return ptr;
	}

	FORCE_INLINE void aligned_free(void *ptr)
	{
		free(ptr);
	}

	// Stubs: CPUID is not available on ARM/WebAssembly; DetectCPUFeatures() is guarded separately
	FORCE_INLINE void __cpuidex(int* cpuinfo, int function, int subfunction)
	{
		(void)function; (void)subfunction;
		cpuinfo[0] = cpuinfo[1] = cpuinfo[2] = cpuinfo[3] = 0;
	}

	FORCE_INLINE unsigned long long _xgetbv(unsigned int index)
	{
		(void)index;
		return 0;
	}

#else // x86: GCC / clang
	#include <cpuid.h>
#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
	#include <malloc/malloc.h> // memalign
#else
	#include <malloc.h> // memalign
#endif
	#include <mm_malloc.h>
	#include <immintrin.h>
	#include <new>

	#define FORCE_INLINE inline

	FORCE_INLINE unsigned long find_clear_lsb(unsigned int *mask)
	{
		unsigned long idx;
		idx = __builtin_ctzl(*mask);
		*mask &= *mask - 1;
		return idx;
	}

	FORCE_INLINE void *aligned_alloc(size_t alignment, size_t size)
	{
		return memalign(alignment, size);
	}

	FORCE_INLINE void aligned_free(void *ptr)
	{
		free(ptr);
	}

	// GCC 11+ and clang 19+ provide __cpuidex in <cpuid.h>; older ones only have __cpuid_count
#if (defined(__clang__) && __clang_major__ < 19) || (!defined(__clang__) && defined(__GNUC__) && __GNUC__ < 11)
	FORCE_INLINE void moc_cpuidex(int* cpuinfo, int function, int subfunction)
	{
		__cpuid_count(function, subfunction, cpuinfo[0], cpuinfo[1], cpuinfo[2], cpuinfo[3]);
	}
	#define __cpuidex moc_cpuidex
#endif

	// GCC 12+ provides _xgetbv via <immintrin.h>; clang always has it
#if !defined(__clang__) && defined(__GNUC__) && __GNUC__ < 12
	FORCE_INLINE unsigned long long _xgetbv(unsigned int index)
	{
		unsigned int eax, edx;
		__asm__ __volatile__(
			"xgetbv;"
			: "=a" (eax), "=d"(edx)
			: "c" (index)
		);
		return ((unsigned long long)edx << 32) | eax;
	}
#endif

#endif // ARM vs x86

#else
	#error Unsupported compiler
#endif
