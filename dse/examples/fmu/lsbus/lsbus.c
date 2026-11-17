// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dse/fmu/fmu.h>
#include <dse/ncodec/codec.h>
#include <dse/ncodec/interface/frame.h>

typedef struct {
    double  counter;
    NCODEC* can_rx;
    NCODEC* can_tx;
} VarTable;

FmuInstanceData* fmu_create(FmuInstanceData* fmu)
{
    VarTable* v = malloc(sizeof(VarTable));
    *v = (VarTable){
        .counter = fmu_register_var(fmu, 1, false, offsetof(VarTable, counter)),
        .can_rx = fmu_lookup_ncodec(fmu, 2, true),
        .can_tx = fmu_lookup_ncodec(fmu, 3, false),
    };
    fmu_register_var_table(fmu, v);

    if (v->can_rx == NULL) {
        fmu_log(fmu, FmiLogError, "Error", "CAN Rx_Data not configured (VR 2)");
    }
    if (v->can_tx == NULL) {
        fmu_log(fmu, FmiLogError, "Error", "CAN Tx_Data not configured (VR 3)");
    }
    return NULL;
}

int fmu_init(FmuInstanceData* fmu)
{
    UNUSED(fmu);
    return 0;
}

int fmu_step(FmuInstanceData* fmu, double CommunicationPoint, double stepSize)
{
    UNUSED(CommunicationPoint);
    UNUSED(stepSize);
    VarTable* v = fmu_var_table(fmu);

    /* Consume CAN frames from the bus (LS-BUS Rx_Data). */
    NCodecCanMessage rx_msg;
    while (ncodec_read(v->can_rx, &rx_msg) >= 0) {
        fmu_log(fmu, FmiLogOk, "Debug", "RX (%03x): %.*s", rx_msg.frame_id,
            (int)rx_msg.len, rx_msg.buffer);
    }

    /* Increment the counter. */
    v->counter += 1;

    /* Send a CAN frame to the bus (LS-BUS Tx_Data). */
    char msg[9] = {};
    snprintf(msg, sizeof(msg), "Cnt %d", (int)v->counter);
    ncodec_write(v->can_tx, &(NCodecCanMessage){
                                .frame_id = 0x100 + ((int)v->counter % 0x100),
                                .frame_type = CAN_BASE_FRAME,
                                .buffer = (uint8_t*)msg,
                                .len = strlen(msg),
                            });
    ncodec_flush(v->can_tx);

    return 0;
}

int fmu_destroy(FmuInstanceData* fmu)
{
    UNUSED(fmu);
    return 0;
}
