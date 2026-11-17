# Copyright 2026 Robert Bosch GmbH
#
# SPDX-License-Identifier: Apache-2.0

# FMI-LS-BUS Codec (temporary home, see FMI3_LS_BUS_PLAN.md WP1).
# Consumers link: lsbus-codec $<TARGET_OBJECTS:lsbus-codec>
add_library(lsbus-codec OBJECT
    codec.c
)
target_include_directories(lsbus-codec
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/headers
        ${FMI3_INCLUDE_DIR}
)
target_link_libraries(lsbus-codec
    PRIVATE
        ab-codec
)
