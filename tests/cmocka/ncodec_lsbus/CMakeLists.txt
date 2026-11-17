# Copyright 2026 Robert Bosch GmbH
#
# SPDX-License-Identifier: Apache-2.0

add_executable(test_lsbus_codec
    test_codec.c
)
target_include_directories(test_lsbus_codec
    PRIVATE
        ${REPO_DIR}
        ${DSE_NCODEC_INCLUDE_DIR}
        ${DSE_CLIB_INCLUDE_DIR}
)
target_compile_definitions(test_lsbus_codec
    PUBLIC
        CMOCKA_TESTING
)
target_link_libraries(test_lsbus_codec
    PRIVATE
        ab-codec
        lsbus-codec
        cmocka
        dl
        m
)
install(TARGETS test_lsbus_codec)
