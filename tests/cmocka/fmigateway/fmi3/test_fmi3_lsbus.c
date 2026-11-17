// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <dse/testing.h>
#include <fmi3Functions.h>
#include <dse/modelc/controller/model_private.h>
#include <dse/ncodec/codec.h>
#include <dse/ncodec/interface/frame.h>
#include <dse/ncodec/interface/pdu.h>
#include <dse/fmigateway/fmigateway.h>


#define STEP_SIZE    0.0005
#define VR_RX        2
#define VR_TX        3
#define VR_STREAM_RX 4
#define VR_STREAM_TX 5
#define VR_TX_CLOCK  7
#define VR_PDU_RX    8
#define VR_PDU_TX    9
#define VR_RX_2      12
#define VR_TX_2      13
#define REMOTE_MIME_TYPE                                                       \
    "application/x-automotive-bus; interface=stream; type=frame; bus=can; "    \
    "schema=fbs; bus_id=1; node_id=9; interface_id=0"
#define REMOTE_PDU_MIME_TYPE                                                   \
    "application/x-automotive-bus; interface=stream; type=pdu; schema=fbs; "   \
    "ecu_id=9; swc_id=9"


void stub_setup_objects(Controller* c, Endpoint* e);
void stub_release_objects(Controller* c, Endpoint* e);


/* CAN Transmit operations: ID 0x10 (0xAA) and ID 0x11 (0xBB). */
static const uint8_t op_1[] = { 0x10, 0, 0, 0, 17, 0, 0, 0, 0x10, 0, 0, 0, 0, 0,
    1, 0, 0xAA };
static const uint8_t op_2[] = { 0x10, 0, 0, 0, 17, 0, 0, 0, 0x11, 0, 0, 0, 0, 0,
    1, 0, 0xBB };


typedef struct {
    Controller*      controller;
    Endpoint*        endpoint;
    FmuInstanceData* inst;
    /* Other SimBus nodes, on the "can" and "can_pdu" signals. */
    FmuSignalVector  view;
    NCODEC*          remote;
    NCODEC*          remote_pdu;
} LsBusTest;


static NCODEC* _remote(LsBusTest* t, const char* signal, const char* mime_type)
{
    FmiGateway* fmi_gw = t->inst->data;
    for (SignalVector* sv = fmi_gw->model->sv; sv && sv->name; sv++) {
        for (uint32_t i = 0; sv->is_binary && i < sv->count; i++) {
            if (strcmp(sv->signal[i], signal)) continue;
            t->view = (FmuSignalVector){
                .count = sv->count,
                .binary = sv->binary,
                .length = sv->length,
                .buffer_size = sv->buffer_size,
            };
            NCODEC* nc = ncodec_create(mime_type);
            ((NCodecInstance*)nc)->stream = fmu_sv_stream_create(&t->view, i);
            return nc;
        }
    }
    return NULL;
}


static int _setup(void** state)
{
    LsBusTest* t = calloc(1, sizeof(LsBusTest));
    t->controller = calloc(1, sizeof(Controller));
    t->controller->adapter = calloc(1, sizeof(Adapter));
    t->endpoint = calloc(1, sizeof(Endpoint));
    stub_setup_objects(t->controller, t->endpoint);

    t->inst = fmi3InstantiateCoSimulation("test_inst",
        "{880ba27a-938d-4e61-9817-2ae18767232a}",
        "../../../../tests/cmocka/fmigateway/fmi3/data/fmi3_lsbus/resources",
        false, false, false, false, NULL, 0, NULL, NULL, NULL);
    assert_non_null(t->inst);
    assert_int_equal(fmi3ExitInitializationMode(t->inst), fmi3OK);

    t->remote = _remote(t, "can", REMOTE_MIME_TYPE);
    t->remote_pdu = _remote(t, "can_pdu", REMOTE_PDU_MIME_TYPE);
    assert_non_null(t->remote);
    assert_non_null(t->remote_pdu);

    *state = t;
    return 0;
}


static int _teardown(void** state)
{
    LsBusTest* t = *state;
    ncodec_close(t->remote);
    ncodec_close(t->remote_pdu);
    fmi3FreeInstance(t->inst);
    stub_release_objects(t->controller, t->endpoint);
    free(t->controller->adapter);
    free(t->controller);
    free(t->endpoint);
    free(t);
    return 0;
}


static void _do_step(fmi3Instance inst, double t)
{
    fmi3Boolean event, terminate, early;
    fmi3Float64 last;
    assert_int_equal(fmi3DoStep(inst, t, STEP_SIZE, false, &event, &terminate,
                         &early, &last),
        fmi3OK);
}


static void _set_binary(
    fmi3Instance inst, fmi3ValueReference vr, const uint8_t* data, size_t len)
{
    fmi3Binary value = data;
    assert_int_equal(fmi3SetBinary(inst, &vr, 1, &len, &value, 1), fmi3OK);
}


static size_t _get_binary(
    fmi3Instance inst, fmi3ValueReference vr, fmi3Binary* value)
{
    size_t len;
    assert_int_equal(fmi3GetBinary(inst, &vr, 1, &len, value, 1), fmi3OK);
    return len;
}


void test_fmigateway__fmi3_lsbus_bridge(void** state)
{
    LsBusTest*       t = *state;
    fmi3Binary       value;
    NCodecCanMessage msg;

    /* Importer sends 0x10. */
    const uint8_t rx_op[] = { 0x10, 0, 0, 0, 17, 0, 0, 0, 0x10, 0, 0, 0, 0, 0,
        1, 0, 0xAA };
    _set_binary(t->inst, VR_RX, rx_op, sizeof(rx_op));
    fmigateway_lsbus_rx(t->inst);

    /* SimBus (stub does not exchange): the gateway sent 0x10 ... */
    ncodec_seek(t->remote, 0, NCODEC_SEEK_SET);
    assert_int_equal(ncodec_read(t->remote, &msg), 1);
    assert_int_equal(msg.frame_id, 0x10);
    assert_int_equal(msg.sender.node_id, 2);
    assert_int_equal(msg.buffer[0], 0xAA);
    assert_int_equal(ncodec_read(t->remote, &msg), -ENOMSG);
    /* ... and receives 0x20 from another node. */
    ncodec_truncate(t->remote);
    ncodec_write(t->remote, &(NCodecCanMessage){ .frame_id = 0x20,
                                .frame_type = CAN_BASE_FRAME,
                                .buffer = (uint8_t[]){ 0xBB },
                                .len = 1 });
    ncodec_flush(t->remote);
    fmigateway_lsbus_tx(t->inst, true);

    const uint8_t tx_op[] = { 0x10, 0, 0, 0, 17, 0, 0, 0, 0x20, 0, 0, 0, 0, 0,
        1, 0, 0xBB };
    assert_int_equal(_get_binary(t->inst, VR_TX, &value), sizeof(tx_op));
    assert_memory_equal(value, tx_op, sizeof(tx_op));

    /* Next step: nothing sent, nothing received. */
    _do_step(t->inst, STEP_SIZE);
    assert_int_equal(_get_binary(t->inst, VR_TX, &value), 0);
}


void test_fmigateway__fmi3_lsbus_bridge_pdu(void** state)
{
    LsBusTest* t = *state;
    fmi3Binary value;
    NCodecPdu  pdu;

    /* Importer sends 0x10. */
    const uint8_t rx_op[] = { 0x10, 0, 0, 0, 17, 0, 0, 0, 0x10, 0, 0, 0, 0, 0,
        1, 0, 0xAA };
    _set_binary(t->inst, VR_PDU_RX, rx_op, sizeof(rx_op));
    fmigateway_lsbus_rx(t->inst);

    /* SimBus (stub does not exchange): the gateway sent a CAN PDU ... */
    ncodec_seek(t->remote_pdu, 0, NCODEC_SEEK_SET);
    assert_int_equal(ncodec_read(t->remote_pdu, &pdu), 1);
    assert_int_equal(pdu.id, 0x10);
    assert_int_equal(pdu.swc_id, 78);
    assert_int_equal(pdu.ecu_id, 7);
    assert_int_equal(pdu.transport_type, NCodecPduTransportTypeCan);
    assert_int_equal(
        pdu.transport.can_message.frame_format, NCodecPduCanFrameFormatBase);
    assert_int_equal(pdu.payload[0], 0xAA);
    assert_int_equal(ncodec_read(t->remote_pdu, &pdu), -ENOMSG);
    /* ... and receives an extended CAN PDU and a PDU without CAN transport. */
    ncodec_truncate(t->remote_pdu);
    NCodecPdu can_pdu = {
        .id = 0x1234567,
        .payload = (uint8_t[]){ 0xBB },
        .payload_len = 1,
        .transport_type = NCodecPduTransportTypeCan,
        .transport.can_message.frame_format = NCodecPduCanFrameFormatExtended,
    };
    ncodec_write(t->remote_pdu, &can_pdu);
    ncodec_write(t->remote_pdu,
        &(NCodecPdu){
            .id = 0x30, .payload = (uint8_t[]){ 0xCC }, .payload_len = 1 });
    ncodec_flush(t->remote_pdu);
    fmigateway_lsbus_tx(t->inst, true);

    /* Tx_Data: only the CAN PDU, as an extended frame (ide=1). */
    const uint8_t tx_op[] = { 0x10, 0, 0, 0, 17, 0, 0, 0, 0x67, 0x45, 0x23,
        0x01, 1, 0, 1, 0, 0xBB };
    assert_int_equal(_get_binary(t->inst, VR_PDU_TX, &value), sizeof(tx_op));
    assert_memory_equal(value, tx_op, sizeof(tx_op));

    /* Next step: nothing sent, nothing received. */
    _do_step(t->inst, STEP_SIZE);
    assert_int_equal(_get_binary(t->inst, VR_PDU_TX, &value), 0);
}


void test_fmigateway__fmi3_lsbus_format_error(void** state)
{
    LsBusTest* t = *state;
    fmi3Binary value;

    const uint8_t unknown_op[] = { 0x99, 0, 0, 0, 9, 0, 0, 0, 0xFF };
    _set_binary(t->inst, VR_RX, unknown_op, sizeof(unknown_op));
    _do_step(t->inst, 0.0);

    const uint8_t format_error[] = { 0x01, 0, 0, 0, 19, 0, 0, 0, 9, 0, 0x99, 0,
        0, 0, 9, 0, 0, 0, 0xFF };
    assert_int_equal(_get_binary(t->inst, VR_TX, &value), sizeof(format_error));
    assert_memory_equal(value, format_error, sizeof(format_error));
}


void test_fmigateway__fmi3_lsbus_stream_unchanged(void** state)
{
    LsBusTest* t = *state;
    fmi3Binary value;

    /* Signal without dse.standards.fmi-ls-bus.enabled: bytes pass as is. */
    _set_binary(t->inst, VR_STREAM_RX, (const uint8_t*)"RAW", 3);
    _do_step(t->inst, 0.0);
    assert_int_equal(_get_binary(t->inst, VR_STREAM_TX, &value), 3);
    assert_memory_equal(value, "RAW", 3);
}


void test_fmigateway__fmi3_lsbus_clock(void** state)
{
    LsBusTest*               t = *state;
    const fmi3ValueReference vr = VR_TX_CLOCK;
    fmi3Float64              interval = 0;
    fmi3IntervalQualifier    qualifier;

    assert_int_equal(
        fmi3GetIntervalDecimal(t->inst, &vr, 1, &interval, &qualifier), fmi3OK);
    assert_double_equal(interval, STEP_SIZE, 0.0);
}


void test_fmigateway__fmi3_lsbus_terminals(void** state)
{
    LsBusTest* t = *state;
    fmi3Binary value;

    /* Terminal 1 sends 0x10, terminal 2 sends 0x11 (same SimBus signal). */
    _set_binary(t->inst, VR_RX, op_1, sizeof(op_1));
    _set_binary(t->inst, VR_RX_2, op_2, sizeof(op_2));
    fmigateway_lsbus_rx(t->inst);
    fmigateway_lsbus_tx(t->inst, true);

    /* Each terminal receives the operation of the other, not its own. */
    assert_int_equal(_get_binary(t->inst, VR_TX, &value), sizeof(op_2));
    assert_memory_equal(value, op_2, sizeof(op_2));
    assert_int_equal(_get_binary(t->inst, VR_TX_2, &value), sizeof(op_1));
    assert_memory_equal(value, op_1, sizeof(op_1));
}


void test_fmigateway__fmi3_lsbus_gateway_behind(void** state)
{
    LsBusTest*       t = *state;
    NCodecCanMessage msg;

    /* Step not sent: the operation stays on the SimBus ... */
    _set_binary(t->inst, VR_RX, op_1, sizeof(op_1));
    fmigateway_lsbus_rx(t->inst);
    fmigateway_lsbus_tx(t->inst, false);
    /* ... and the next step sends both. */
    _set_binary(t->inst, VR_RX, op_2, sizeof(op_2));
    fmigateway_lsbus_rx(t->inst);

    ncodec_seek(t->remote, 0, NCODEC_SEEK_SET);
    assert_int_equal(ncodec_read(t->remote, &msg), 1);
    assert_int_equal(msg.frame_id, 0x10);
    assert_int_equal(ncodec_read(t->remote, &msg), 1);
    assert_int_equal(msg.frame_id, 0x11);
    assert_int_equal(ncodec_read(t->remote, &msg), -ENOMSG);
}


int run_fmigateway__fmi3_lsbus_tests(void)
{
    void* s = _setup;
    void* t = _teardown;

    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(
            test_fmigateway__fmi3_lsbus_bridge, s, t),
        cmocka_unit_test_setup_teardown(
            test_fmigateway__fmi3_lsbus_bridge_pdu, s, t),
        cmocka_unit_test_setup_teardown(
            test_fmigateway__fmi3_lsbus_format_error, s, t),
        cmocka_unit_test_setup_teardown(
            test_fmigateway__fmi3_lsbus_stream_unchanged, s, t),
        cmocka_unit_test_setup_teardown(
            test_fmigateway__fmi3_lsbus_clock, s, t),
        cmocka_unit_test_setup_teardown(
            test_fmigateway__fmi3_lsbus_terminals, s, t),
        cmocka_unit_test_setup_teardown(
            test_fmigateway__fmi3_lsbus_gateway_behind, s, t),
    };

    return cmocka_run_group_tests_name("fmi3_lsbus", tests, NULL, NULL);
}
