if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86" AND NOT EMSCRIPTEN)
    set(MOC_SOURCES
        MaskedOcclusionCulling.cpp
        MaskedOcclusionCullingAVX2.cpp
        MaskedOcclusionCullingAVX512.cpp
    )
else()
    # ARM/WebAssembly/other: base file only (SSE2/SSE4.1 via sse2neon on ARM, SIMD128 on wasm)
    set(MOC_SOURCES
        MaskedOcclusionCulling.cpp
    )
endif()

add_library(maskedoc STATIC ${MOC_SOURCES})
target_include_directories(maskedoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(EMSCRIPTEN)
    # WebAssembly: SSE4.1 intrinsics are emulated on top of wasm SIMD128.
    set_source_files_properties(MaskedOcclusionCulling.cpp PROPERTIES COMPILE_FLAGS "-msimd128 -msse4.1")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    if(MSVC)
        # Main file must be compiled at SSE2 baseline (no AVX)
        set_source_files_properties(MaskedOcclusionCulling.cpp PROPERTIES COMPILE_FLAGS "/arch:SSE2")
        set_source_files_properties(MaskedOcclusionCullingAVX2.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")
        set_source_files_properties(MaskedOcclusionCullingAVX512.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX512")
    else()
        set_source_files_properties(MaskedOcclusionCulling.cpp PROPERTIES COMPILE_FLAGS "-msse4.1 -mxsave")
        set_source_files_properties(MaskedOcclusionCullingAVX2.cpp PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
        set_source_files_properties(MaskedOcclusionCullingAVX512.cpp PROPERTIES COMPILE_FLAGS "-mavx512f -mavx512bw")
    endif()
endif()
