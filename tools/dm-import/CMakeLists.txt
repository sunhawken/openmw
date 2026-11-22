# brutalmorrowind-import builds the Dark Messiah data from the user's own game
# files. The launcher runs it from the folder of its own executable.
#
# The program is a small native wrapper (launcher.cpp) that starts dm_import.py
# with the Python runtime in dm-import/python. package_python.py builds that
# runtime (standalone CPython with numpy and Pillow); BRUTAL_PYTHON_RUNTIME
# points the build at it. Without it the wrapper uses BRUTAL_IMPORT_PYTHON or
# python3, which is enough for development builds.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(BRUTAL_PYTHON_RUNTIME "" CACHE PATH "Python runtime made by tools/dm-import/package_python.py")

# Staged here, then copied next to the executable after each build (the build
# tree and both packages keep dm-import beside the programs).
set(DM_IMPORT_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/stage")
file(GLOB_RECURSE DM_IMPORT_SOURCES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/*.json" "${CMAKE_CURRENT_SOURCE_DIR}/*.txt")
list(FILTER DM_IMPORT_SOURCES EXCLUDE REGEX "^(third_party|\\.cache)/|__pycache__|^CMakeLists.txt$")
foreach (f ${DM_IMPORT_SOURCES})
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${f}" "${DM_IMPORT_BUILD_DIR}/${f}" COPYONLY)
endforeach ()

add_custom_command(
    OUTPUT "${DM_IMPORT_BUILD_DIR}/third_party/SourceIO/.commit" "${DM_IMPORT_BUILD_DIR}/third_party/io_scene_mw/.commit"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fetch_dependencies.py" "${DM_IMPORT_BUILD_DIR}/third_party"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.json"
    COMMENT "Fetching the importer's pinned third-party libraries")
add_custom_target(dm-import-dependencies ALL
    DEPENDS "${DM_IMPORT_BUILD_DIR}/third_party/SourceIO/.commit" "${DM_IMPORT_BUILD_DIR}/third_party/io_scene_mw/.commit")

if (BRUTAL_PYTHON_RUNTIME)
    add_custom_command(OUTPUT "${DM_IMPORT_BUILD_DIR}/python/.copied"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${BRUTAL_PYTHON_RUNTIME}" "${DM_IMPORT_BUILD_DIR}/python"
        COMMAND "${CMAKE_COMMAND}" -E touch "${DM_IMPORT_BUILD_DIR}/python/.copied"
        COMMENT "Copying the importer's Python runtime")
    add_custom_target(dm-import-python ALL DEPENDS "${DM_IMPORT_BUILD_DIR}/python/.copied")
endif ()

if (WIN32)
    set(DM_IMPORT_INSTALL_DIR "dm-import")
    set(DM_IMPORT_BIN_DIR ".")
    set(DM_IMPORT_RELATIVE "dm-import")
elseif (UNIX AND NOT APPLE)
    set(DM_IMPORT_INSTALL_DIR "${DATADIR}/dm-import")
    set(DM_IMPORT_BIN_DIR "${BINDIR}")
    file(RELATIVE_PATH DM_IMPORT_RELATIVE "${BINDIR}" "${DM_IMPORT_INSTALL_DIR}")
endif ()

openmw_add_executable(brutalmorrowind-import launcher.cpp)
target_compile_definitions(brutalmorrowind-import PRIVATE DM_IMPORT_RELATIVE="${DM_IMPORT_RELATIVE}")
add_dependencies(brutalmorrowind-import dm-import-dependencies)
if (BRUTAL_PYTHON_RUNTIME)
    add_dependencies(brutalmorrowind-import dm-import-python)
endif ()
add_custom_target(dm-import-place ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${DM_IMPORT_BUILD_DIR}" "$<TARGET_FILE_DIR:brutalmorrowind-import>/dm-import"
    DEPENDS brutalmorrowind-import
    COMMENT "Placing the importer next to brutalmorrowind-import")

if (DEFINED DM_IMPORT_INSTALL_DIR)
    install(TARGETS brutalmorrowind-import RUNTIME DESTINATION "${DM_IMPORT_BIN_DIR}")
    install(DIRECTORY "${DM_IMPORT_BUILD_DIR}/" DESTINATION "${DM_IMPORT_INSTALL_DIR}"
        PATTERN "__pycache__" EXCLUDE PATTERN ".copied" EXCLUDE)
endif ()
