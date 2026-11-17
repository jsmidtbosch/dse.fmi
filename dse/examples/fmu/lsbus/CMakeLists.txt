# Copyright 2026 Robert Bosch GmbH
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.21)

set(MODULE "LsBus")
set(MODULE_LC "lsbus")
project(${MODULE}
    VERSION ${VERSION}
    DESCRIPTION "Dynamic Simulation Environment - FMU FMI-LS-BUS Example"
    HOMEPAGE_URL "$ENV{PROJECT_URL}"
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -ggdb -DLIBXML_STATIC")
file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/.keepme
    CONTENT "keepme"
)


# Targets - FMI3 FMU
# ==================
set(FMU_FMI3_PATH examples/fmu/${MODULE_LC}/fmi3)
add_library(fmu3${MODULE_LC} SHARED
    lsbus.c
)
target_include_directories(fmu3${MODULE_LC}
    PRIVATE
        ${REPO_DIR}
        ${DSE_CLIB_INCLUDE_DIR}
        ${DSE_MODELC_INCLUDE_DIR}
)
target_link_libraries(fmu3${MODULE_LC}
    PRIVATE
        fmi3-common
        ab-codec
        dse_external_xml2
        $<$<BOOL:${WIN32}>:bcrypt>
        $<$<BOOL:${WIN32}>:dse_external_dl>
        m
)
set_target_properties(fmu3${MODULE_LC}
    PROPERTIES
        PREFIX ""
)
install(
    TARGETS
        fmu3${MODULE_LC}
    LIBRARY DESTINATION
        ${FMU_FMI3_PATH}/binaries/x86_64-linux
)
install(
    FILES
        fmi3/modelDescription.xml
    DESTINATION
        ${FMU_FMI3_PATH}
)
install(
    DIRECTORY
        fmi3/terminalsAndIcons
        fmi3/extra
    DESTINATION
        ${FMU_FMI3_PATH}
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/.keepme
    DESTINATION
        ${FMU_FMI3_PATH}/resources
)


# Package FMU
# ===========
install(CODE "
    execute_process(
        COMMAND sh -c \"
            FILE=${CMAKE_CURRENT_BINARY_DIR}/fmu3${MODULE_LC}.zip
            if [ ! -f $FILE ]; then
                zip -r $FILE ./
            fi
        \"
        WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/${FMU_FMI3_PATH}
    )"
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/fmu3${MODULE_LC}.zip
    DESTINATION
        ${FMU_FMI3_PATH}
    RENAME
        ${MODULE_LC}.fmu
)
