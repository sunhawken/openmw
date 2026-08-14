set(RESUMELAUNCHER
    resumelauncher.cpp
)

if (WIN32)
    list(APPEND RESUMELAUNCHER ${CMAKE_SOURCE_DIR}/files/windows/resumelauncher.rc)
endif()

source_group(apps\\resumelauncher FILES ${RESUMELAUNCHER})

# GUI app (WIN32) - no console window, shows a message box on error instead
openmw_add_executable(resumelauncher WIN32
    ${RESUMELAUNCHER}
)

if (WIN32)
    target_link_libraries(resumelauncher shell32 ole32)
    install(TARGETS resumelauncher RUNTIME DESTINATION ".")
    # The project defines NOMB globally to avoid a name clash with
    # MWGui::MessageBox in the main engine - doesn't apply here, this
    # standalone tool has no MWGui dependency and needs the real
    # MessageBoxW/MB_OK/MB_ICONERROR from windows.h.
    if (MSVC)
        target_compile_options(resumelauncher PRIVATE /UNOMB)
    endif()
endif()
