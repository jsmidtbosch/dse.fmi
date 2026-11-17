// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <dse/testing.h>
#include <dse/ncodec/codec.h>
#include <dse/ncodec/interface/frame.h>
#include <dse/fmu/fmu.h>
#include <fmi3Functions.h>


#define STEP_SIZE 0.0005
#define VR_RX     2
#define VR_TX     3
#define VR_RX_CLK 4
#define VR_TX_CLK 5
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif


uint8_t __log_level__; /* LOG_ERROR LOG_INFO LOG_DEBUG LOG_TRACE */


/* Test model: sends one frame (on the first step), records received frames. */
typedef struct {
    double   counter;
    NCODEC*  rx;
    NCODEC*  tx;
    uint32_t rx_id[4];
    uint8_t  rx_data[4];
    size_t   rx_count;
} VarTable;

FmuInstanceData* fmu_create(FmuInstanceData* fmu)
{
    VarTable* v = calloc(1, sizeof(VarTable));
    v->counter = fmu_register_var(fmu, 1, false, offsetof(VarTable, counter));
    v->rx = fmu_lookup_ncodec(fmu, VR_RX, true);
    v->tx = fmu_lookup_ncodec(fmu, VR_TX, false);
    fmu_register_var_table(fmu, v);
    return NULL;
}

int32_t fmu_init(FmuInstanceData* fmu)
{
    UNUSED(fmu);
    return 0;
}

int32_t fmu_step(
    FmuInstanceData* fmu, double communication_point, double step_size)
{
    UNUSED(communication_point);
    UNUSED(step_size);
    VarTable* v = fmu_var_table(fmu);

    NCodecCanMessage msg;
    while (ncodec_read(v->rx, &msg) >= 0) {
        if (v->rx_count >= ARRAY_SIZE(v->rx_id)) continue;
        v->rx_id[v->rx_count] = msg.frame_id;
        v->rx_data[v->rx_count] = msg.len ? msg.buffer[0] : 0;
        v->rx_count++;
    }
    if (v->counter == 0) {
        uint8_t payload = fmu->instance.name[0];
        ncodec_write(v->tx, &(NCodecCanMessage){ .frame_id = payload << 4,
                                .frame_type = CAN_BASE_FRAME,
                                .buffer = &payload,
                                .len = 1 });
        ncodec_flush(v->tx);
    }
    v->counter += 1;
    return 0;
}

int32_t fmu_destroy(FmuInstanceData* fmu)
{
    UNUSED(fmu);
    return 0;
}


static fmi3Instance _instantiate(const char* name)
{
    fmi3Instance inst = fmi3InstantiateCoSimulation(name, "{token}",
        "data/test_lsbus/resources", false, false, true, false, NULL, 0, NULL,
        NULL, NULL);
    assert_non_null(inst);
    assert_non_null(fmu_var_table(inst));
    assert_non_null(((VarTable*)fmu_var_table(inst))->rx);
    assert_non_null(((VarTable*)fmu_var_table(inst))->tx);
    assert_int_equal(fmi3ExitInitializationMode(inst), fmi3OK);
    return inst;
}


static void _do_step(fmi3Instance inst, double t)
{
    fmi3Boolean event = true, terminate = true, early = true;
    fmi3Float64 last = 0;
    assert_int_equal(fmi3DoStep(inst, t, STEP_SIZE, false, &event, &terminate,
                         &early, &last),
        fmi3OK);
    assert_false(event);
    assert_false(terminate);
    assert_false(early);
    assert_double_equal(last, t + STEP_SIZE, 0.0);
}


static void _update_discrete_states(fmi3Instance inst)
{
    fmi3Boolean b[5] = { true, true, true, true, true };
    fmi3Float64 next = 0;
    assert_int_equal(fmi3UpdateDiscreteStates(
                         inst, &b[0], &b[1], &b[2], &b[3], &b[4], &next),
        fmi3OK);
    for (size_t i = 0; i < ARRAY_SIZE(b); i++) {
        assert_false(b[i]);
    }
}


/* Tx_Clock tick: exchange A.Tx -> B.Rx and B.Tx -> A.Rx in Event Mode. */
static void _event(fmi3Instance a, fmi3Instance b, size_t* a_size,
    size_t* b_size, uint8_t* a_tx, size_t a_tx_len)
{
    const fmi3ValueReference rx = VR_RX, tx = VR_TX, rx_clk = VR_RX_CLK;
    fmi3Binary               a_data, b_data;
    fmi3Clock                tick = fmi3ClockActive;

    fmi3EnterEventMode(a, false, false, NULL, 0, true);
    fmi3EnterEventMode(b, false, false, NULL, 0, true);

    assert_int_equal(fmi3GetBinary(a, &tx, 1, a_size, &a_data, 1), fmi3OK);
    if (a_tx) {
        assert_int_equal(*a_size, a_tx_len);
        assert_memory_equal(a_data, a_tx, a_tx_len);
    }
    /* Set before Get on B: must not discard B.Tx. */
    assert_int_equal(fmi3SetBinary(b, &rx, 1, a_size, &a_data, 1), fmi3OK);
    assert_int_equal(fmi3SetClock(b, &rx_clk, 1, &tick), fmi3OK);
    assert_int_equal(fmi3GetBinary(b, &tx, 1, b_size, &b_data, 1), fmi3OK);
    assert_int_equal(fmi3SetBinary(a, &rx, 1, b_size, &b_data, 1), fmi3OK);
    assert_int_equal(fmi3SetClock(a, &rx_clk, 1, &tick), fmi3OK);

    _update_discrete_states(a);
    _update_discrete_states(b);
    assert_int_equal(fmi3EnterStepMode(a), fmi3OK);
    assert_int_equal(fmi3EnterStepMode(b), fmi3OK);
}


void test_fmi3lsbus_exchange(void** state)
{
    UNUSED(state);
    fmi3Instance a = _instantiate("A");
    fmi3Instance b = _instantiate("B");
    VarTable*    va = fmu_var_table(a);
    VarTable*    vb = fmu_var_table(b);
    size_t       a_size, b_size;

    /* Step 1: each model sends one frame. */
    _do_step(a, 0.0);
    _do_step(b, 0.0);
    uint8_t a_tx[] = {
        0x10, 0, 0, 0, 17, 0, 0, 0, /* CAN Transmit, Length. */
        0x10, 0x04, 0, 0,           /* ID = 'A' << 4. */
        0, 0, 1, 0, 'A',            /* Ide, Rtr, DL, Data. */
    };
    _event(a, b, &a_size, &b_size, a_tx, sizeof(a_tx));
    assert_int_equal(b_size, 17);

    /* Step 2: frames are received, nothing is sent. */
    _do_step(a, STEP_SIZE);
    _do_step(b, STEP_SIZE);
    assert_int_equal(va->rx_count, 1);
    assert_int_equal(va->rx_id[0], 'B' << 4);
    assert_int_equal(va->rx_data[0], 'B');
    assert_int_equal(vb->rx_count, 1);
    assert_int_equal(vb->rx_id[0], 'A' << 4);
    assert_int_equal(vb->rx_data[0], 'A');
    _event(a, b, &a_size, &b_size, NULL, 0);
    assert_int_equal(a_size, 0);
    assert_int_equal(b_size, 0);

    /* Step 3: nothing is received again. */
    _do_step(a, 2 * STEP_SIZE);
    _do_step(b, 2 * STEP_SIZE);
    assert_int_equal(va->rx_count, 1);
    assert_int_equal(vb->rx_count, 1);
    assert_double_equal(va->counter, 3, 0.0);

    fmi3FreeInstance(a);
    fmi3FreeInstance(b);
}


void test_fmi3lsbus_clocks(void** state)
{
    UNUSED(state);
    fmi3Instance             inst = _instantiate("A");
    const fmi3ValueReference tx_clk = VR_TX_CLK, rx_clk = VR_RX_CLK;
    fmi3Float64              interval = 0, shift = 0;
    fmi3IntervalQualifier    qualifier = fmi3IntervalNotYetKnown;

    assert_int_equal(
        fmi3GetIntervalDecimal(inst, &tx_clk, 1, &interval, &qualifier),
        fmi3OK);
    assert_double_equal(interval, 0.0005, 0.0);
    assert_int_equal(qualifier, fmi3IntervalUnchanged);
    assert_int_equal(fmi3GetShiftDecimal(inst, &tx_clk, 1, &shift), fmi3OK);
    assert_double_equal(shift, 0.001, 0.0);
    /* Triggered Clock: no interval. */
    assert_int_equal(
        fmi3GetIntervalDecimal(inst, &rx_clk, 1, &interval, &qualifier),
        fmi3Error);

    fmi3FreeInstance(inst);
}


void test_fmi3lsbus_format_error(void** state)
{
    UNUSED(state);
    fmi3Instance             inst = _instantiate("A");
    const fmi3ValueReference rx = VR_RX, tx = VR_TX;
    const uint8_t            unknown_op[] = { 0x99, 0, 0, 0, 9, 0, 0, 0, 0xFF };
    const uint8_t*           rx_data = unknown_op;
    size_t                   rx_size = sizeof(unknown_op);
    fmi3Binary               tx_data = NULL;
    size_t                   tx_size = 0;

    fmi3EnterEventMode(inst, false, false, NULL, 0, true);
    assert_int_equal(
        fmi3SetBinary(inst, &rx, 1, &rx_size, &rx_data, 1), fmi3OK);
    _update_discrete_states(inst);
    assert_int_equal(fmi3EnterStepMode(inst), fmi3OK);
    _do_step(inst, 0.0);
    fmi3EnterEventMode(inst, false, false, NULL, 0, true);
    assert_int_equal(
        fmi3GetBinary(inst, &tx, 1, &tx_size, &tx_data, 1), fmi3OK);

    /* Format Error (19 bytes) followed by the model's CAN Transmit. */
    const uint8_t format_error[] = { 0x01, 0, 0, 0, 19, 0, 0, 0, 9, 0 };
    assert_int_equal(tx_size, 19 + 17);
    assert_memory_equal(tx_data, format_error, sizeof(format_error));
    assert_memory_equal(tx_data + 10, unknown_op, sizeof(unknown_op));
    assert_int_equal(tx_data[19], 0x10);
    assert_int_equal(((VarTable*)fmu_var_table(inst))->rx_count, 0);

    fmi3FreeInstance(inst);
}


int main()
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_fmi3lsbus_exchange),
        cmocka_unit_test(test_fmi3lsbus_clocks),
        cmocka_unit_test(test_fmi3lsbus_format_error),
    };
    return cmocka_run_group_tests_name("test_fmi3lsbus", tests, NULL, NULL);
}
