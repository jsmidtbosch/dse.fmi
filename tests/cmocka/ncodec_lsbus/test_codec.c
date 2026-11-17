// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <dse/testing.h>
#include <stdint.h>
#include <string.h>
#include <dse/ncodec/codec.h>
#include <dse/ncodec/stream/stream.h>
#include <dse/ncodec/interface/frame.h>
#include <dse/ncodec_lsbus/codec.h>


#define UNUSED(x)     ((void)x)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define MIME_TYPE                                                              \
    "application/org.fmi-standard.fmi-ls-bus.can; version=\"1.0.0\""


static NCODEC* _open(const char* mime_type)
{
    NCODEC* nc = lsbus_codec_create(mime_type);
    if (nc) ((NCodecInstance*)nc)->stream = ncodec_buffer_stream_create(512);
    return nc;
}


static size_t _bytes(NCODEC* nc, uint8_t** data)
{
    size_t len = 0;
    ncodec_seek(nc, 0, NCODEC_SEEK_SET);
    ((NCodecInstance*)nc)->stream->read(nc, data, &len, NCODEC_POS_NC);
    return len;
}


static void _put(NCODEC* nc, const uint8_t* data, size_t len)
{
    ncodec_truncate(nc);
    ((NCodecInstance*)nc)->stream->write(nc, (uint8_t*)data, len);
    ncodec_seek(nc, 0, NCODEC_SEEK_SET);
}


void test_lsbus_create(void** state)
{
    UNUSED(state);
    const char* rejected[] = {
        NULL,
        "application/x-automotive-bus; interface=stream; type=frame; bus=can",
        "application/org.fmi-standard.fmi-ls-bus.can",
        "application/org.fmi-standard.fmi-ls-bus.can; version=\"1.3.0\"",
        "application/org.fmi-standard.fmi-ls-bus.lin; version=\"1.0.0\"",
    };
    for (size_t i = 0; i < ARRAY_SIZE(rejected); i++) {
        assert_null(lsbus_codec_create(rejected[i]));
    }

    NCODEC* nc = lsbus_codec_create(
        MIME_TYPE "; bus_id=3; node_id=7; foo=bar; version=1.0.0");
    assert_non_null(nc);
    const char* expect[][2] = {
        { "type", "frame" },
        { "bus", "can" },
        { "version", "1.0.0" },
        { "bus_id", "3" },
        { "node_id", "7" },
        { "interface_id", "0" },
    };
    for (int i = 0; i < (int)ARRAY_SIZE(expect); i++) {
        int              idx = i;
        NCodecConfigItem ci = ncodec_stat(nc, &idx);
        assert_string_equal(ci.name, expect[i][0]);
        assert_string_equal(ci.value, expect[i][1]);
    }
    int idx = ARRAY_SIZE(expect);
    assert_null(ncodec_stat(nc, &idx).name);
    assert_int_equal(idx, -1);
    ncodec_close(nc);
}


void test_lsbus_write_golden(void** state)
{
    UNUSED(state);
    NCODEC* nc = _open(MIME_TYPE);

    uint8_t payload[] = { 0xAA, 0xBB, 0xCC };
    ncodec_write(nc, &(NCodecCanMessage){ .frame_id = 0x123,
                         .frame_type = CAN_BASE_FRAME,
                         .buffer = payload,
                         .len = 3 });
    ncodec_write(nc, &(NCodecCanMessage){ .frame_id = 0x1ABCDEF,
                         .frame_type = CAN_FD_EXTENDED_FRAME,
                         .buffer = payload,
                         .len = 2 });
    uint8_t* data;
    assert_int_equal(_bytes(nc, &data), 0); /* Not flushed. */
    assert_int_equal(ncodec_flush(nc), 19 + 19);

    const uint8_t expect[] = {
        /* CAN Transmit: OpCode, Length, ID, Ide, Rtr, DL, Data. */
        0x10,
        0,
        0,
        0,
        19,
        0,
        0,
        0,
        0x23,
        0x01,
        0,
        0,
        0,
        0,
        3,
        0,
        0xAA,
        0xBB,
        0xCC,
        /* CAN FD Transmit: OpCode, Length, ID, Ide, Brs, Esi, DL, Data. */
        0x11,
        0,
        0,
        0,
        19,
        0,
        0,
        0,
        0xEF,
        0xCD,
        0xAB,
        0x01,
        1,
        1,
        0,
        2,
        0,
        0xAA,
        0xBB,
    };
    assert_int_equal(_bytes(nc, &data), sizeof(expect));
    assert_memory_equal(data, expect, sizeof(expect));

    /* Truncate discards the stream. */
    ncodec_truncate(nc);
    assert_int_equal(_bytes(nc, &data), 0);
    ncodec_close(nc);
}


void test_lsbus_write_dl(void** state)
{
    UNUSED(state);
    NCODEC* nc = _open(MIME_TYPE);

    uint8_t payload[65] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 1, 2, 3 };
    assert_int_equal(ncodec_write(nc, &(NCodecCanMessage){ .frame_id = 1,
                                          .frame_type = CAN_BASE_FRAME,
                                          .buffer = payload,
                                          .len = 9 }),
        -EINVAL);
    assert_int_equal(ncodec_write(nc, &(NCodecCanMessage){ .frame_id = 1,
                                          .frame_type = CAN_FD_BASE_FRAME,
                                          .buffer = payload,
                                          .len = 65 }),
        -EINVAL);
    /* CAN FD: padded to the next valid data length (9 -> 12). */
    assert_int_equal(ncodec_write(nc, &(NCodecCanMessage){ .frame_id = 2,
                                          .frame_type = CAN_FD_BASE_FRAME,
                                          .buffer = payload,
                                          .len = 9 }),
        9);
    ncodec_flush(nc);
    ncodec_seek(nc, 0, NCODEC_SEEK_SET);

    NCodecCanMessage msg;
    assert_int_equal(ncodec_read(nc, &msg), 12);
    assert_int_equal(msg.frame_id, 2);
    assert_memory_equal(msg.buffer, payload, 9);
    assert_memory_equal(msg.buffer + 9, ((uint8_t[]){ 0, 0, 0 }), 3);
    assert_int_equal(ncodec_read(nc, &msg), -ENOMSG);
    ncodec_close(nc);
}


void test_lsbus_format_error(void** state)
{
    UNUSED(state);
    NCODEC* rx = _open(MIME_TYPE);
    NCODEC* tx = _open(MIME_TYPE);
    NCODEC* ab = ncodec_create(
        "application/x-automotive-bus; interface=stream; type=frame; bus=can; "
        "schema=fbs");
    assert_non_null(ab);
    ((NCodecInstance*)ab)->stream = ncodec_buffer_stream_create(512);

    const uint8_t unknown_op[] = { 0x99, 0, 0, 0, 9, 0, 0, 0, 0xFF };
    const uint8_t truncated_op[] = { 0x10, 0, 0, 0, 99, 0, 0, 0 };
    uint8_t       ops[100];
    size_t        len = 0;
    memcpy(ops + len, unknown_op, sizeof(unknown_op));
    len += sizeof(unknown_op);
    /* Received Format Error, consumed without an answer. */
    memcpy(ops + len, ((uint8_t[]){ 0x01, 0, 0, 0, 10, 0, 0, 0, 0, 0 }), 10);
    len += 10;
    memcpy(ops + len,
        ((uint8_t[]){
            0x10, 0, 0, 0, 17, 0, 0, 0, 0x42, 0, 0, 0, 0, 0, 1, 0, 0x55 }),
        17);
    len += 17;
    memcpy(ops + len, truncated_op, sizeof(truncated_op));
    len += sizeof(truncated_op);

    /* Not linked to an LS-BUS codec: no Format Error is written. */
    lsbus_codec_link(rx, ab);
    _put(rx, ops, len);
    NCodecCanMessage msg;
    while (ncodec_read(rx, &msg) >= 0) {
    }
    uint8_t* data;
    assert_int_equal(_bytes(ab, &data), 0);

    lsbus_codec_link(rx, tx);
    _put(rx, ops, len);
    assert_int_equal(ncodec_read(rx, &msg), 1);
    assert_int_equal(msg.frame_id, 0x42);
    assert_int_equal(ncodec_read(rx, &msg), -ENOMSG);

    const uint8_t expect[] = {
        /* Format Error: OpCode, Length (10 + DL), DL, Data. */
        0x01,
        0,
        0,
        0,
        19,
        0,
        0,
        0,
        9,
        0,
        0x99,
        0,
        0,
        0,
        9,
        0,
        0,
        0,
        0xFF,
        0x01,
        0,
        0,
        0,
        18,
        0,
        0,
        0,
        8,
        0,
        0x10,
        0,
        0,
        0,
        99,
        0,
        0,
        0,
    };
    assert_int_equal(_bytes(tx, &data), sizeof(expect));
    assert_memory_equal(data, expect, sizeof(expect));

    ncodec_close(ab);
    ncodec_close(tx);
    ncodec_close(rx);
}


void test_lsbus_read(void** state)
{
    UNUSED(state);
    NCODEC* nc = _open(MIME_TYPE);

    const uint8_t ops[] = {
        /* Confirm (skipped). */
        0x20,
        0,
        0,
        0,
        12,
        0,
        0,
        0,
        0x01,
        0,
        0,
        0,
        /* CAN Transmit, Ide=1. */
        0x10,
        0,
        0,
        0,
        18,
        0,
        0,
        0,
        0x42,
        0,
        0,
        0,
        1,
        0,
        2,
        0,
        0x01,
        0x02,
        /* CAN Transmit, Rtr=1, DL=4 requested, no Data (len 0). */
        0x10,
        0,
        0,
        0,
        16,
        0,
        0,
        0,
        0x43,
        0,
        0,
        0,
        0,
        1,
        4,
        0,
        /* CAN FD Transmit, Ide=0. */
        0x11,
        0,
        0,
        0,
        18,
        0,
        0,
        0,
        0x44,
        0,
        0,
        0,
        0,
        1,
        0,
        1,
        0,
        0x09,
        /* Unknown op (skipped). */
        0x99,
        0,
        0,
        0,
        9,
        0,
        0,
        0,
        0xFF,
    };
    _put(nc, ops, sizeof(ops));

    NCodecCanMessage msg;
    assert_int_equal(ncodec_read(nc, &msg), 2);
    assert_int_equal(msg.frame_id, 0x42);
    assert_int_equal(msg.frame_type, CAN_EXTENDED_FRAME);
    assert_memory_equal(msg.buffer, ((uint8_t[]){ 0x01, 0x02 }), 2);

    assert_int_equal(ncodec_read(nc, &msg), 0);
    assert_int_equal(msg.frame_id, 0x43);
    assert_int_equal(msg.frame_type, CAN_BASE_FRAME);
    assert_null(msg.buffer);

    assert_int_equal(ncodec_read(nc, &msg), 1);
    assert_int_equal(msg.frame_id, 0x44);
    assert_int_equal(msg.frame_type, CAN_FD_BASE_FRAME);
    assert_int_equal(msg.buffer[0], 0x09);

    assert_int_equal(ncodec_read(nc, &msg), -ENOMSG);
    assert_int_equal(ncodec_read(nc, &msg), -ENOMSG);
    ncodec_close(nc);
}


void test_lsbus_read_malformed(void** state)
{
    UNUSED(state);
    NCODEC*       nc = _open(MIME_TYPE);
    const uint8_t valid[] = { 0x10, 0, 0, 0, 17, 0, 0, 0, 0x01, 0, 0, 0, 0, 0,
        1, 0, 0x55 };
    NCodecCanMessage msg;

    struct {
        const char* name;
        uint8_t     ops[40];
        size_t      len;
    } tc[] = {
        { "short header", { 0x10, 0, 0, 0 }, 4 },
        { "length 0", { 0x10, 0, 0, 0, 0, 0, 0, 0 }, 8 },
        { "length beyond buffer", { 0x10, 0, 0, 0, 99, 0, 0, 0 }, 8 },
        { "data beyond op",
            { 0x10, 0, 0, 0, 16, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 8, 0 }, 16 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(tc); i++) {
        /* A valid op, followed by the malformed op. */
        uint8_t buf[100];
        memcpy(buf, valid, sizeof(valid));
        memcpy(buf + sizeof(valid), tc[i].ops, tc[i].len);
        _put(nc, buf, sizeof(valid) + tc[i].len);

        assert_int_equal(ncodec_read(nc, &msg), 1);
        assert_int_equal(ncodec_read(nc, &msg), -ENOMSG);
        assert_int_equal(ncodec_read(nc, &msg), -ENOMSG);
    }
    ncodec_close(nc);
}


void test_lsbus_roundtrip(void** state)
{
    UNUSED(state);
    NCODEC* nc = _open(MIME_TYPE);

    uint8_t fd_payload[64];
    for (size_t i = 0; i < sizeof(fd_payload); i++)
        fd_payload[i] = i;
    NCodecCanMessage tx[] = {
        { .frame_id = 1, .frame_type = CAN_BASE_FRAME },
        { .frame_id = 2,
            .frame_type = CAN_EXTENDED_FRAME,
            .buffer = fd_payload,
            .len = 8 },
        { .frame_id = 3,
            .frame_type = CAN_FD_BASE_FRAME,
            .buffer = fd_payload,
            .len = 64 },
        { .frame_id = 4,
            .frame_type = CAN_FD_EXTENDED_FRAME,
            .buffer = fd_payload,
            .len = 12 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(tx); i++) {
        assert_int_equal(ncodec_write(nc, &tx[i]), tx[i].len);
    }
    ncodec_flush(nc);
    ncodec_seek(nc, 0, NCODEC_SEEK_SET);

    for (size_t i = 0; i < ARRAY_SIZE(tx); i++) {
        NCodecCanMessage rx;
        assert_int_equal(ncodec_read(nc, &rx), tx[i].len);
        assert_int_equal(rx.frame_id, tx[i].frame_id);
        assert_int_equal(rx.frame_type, tx[i].frame_type);
        if (tx[i].len) assert_memory_equal(rx.buffer, tx[i].buffer, tx[i].len);
    }
    NCodecCanMessage rx;
    assert_int_equal(ncodec_read(nc, &rx), -ENOMSG);
    ncodec_close(nc);
}


int main()
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_lsbus_create),
        cmocka_unit_test(test_lsbus_write_golden),
        cmocka_unit_test(test_lsbus_write_dl),
        cmocka_unit_test(test_lsbus_read),
        cmocka_unit_test(test_lsbus_read_malformed),
        cmocka_unit_test(test_lsbus_format_error),
        cmocka_unit_test(test_lsbus_roundtrip),
    };
    return cmocka_run_group_tests_name("test_lsbus_codec", tests, NULL, NULL);
}
