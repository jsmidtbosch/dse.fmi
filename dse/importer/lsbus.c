// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dse/ncodec/codec.h>
#include <dse/ncodec/interface/frame.h>
#include <dse/ncodec/stream/stream.h>
#include <dse/ncodec_lsbus/codec.h>
#include <dse/importer/importer.h>


/**
FMI-LS-BUS Event Mode
=====================

Minimal importer with integrated (ideal) Bus Simulation for FMI-LS-BUS Bus
Terminals. At each communication point (Tx_Clock periodic, interval = step):

    fmi3EnterEventMode
    fmi3SetClock(Tx_Clock) + fmi3GetBinary(Tx_Data)    (print CAN frames)
    fmi3SetBinary(Rx_Data) + fmi3SetClock(Rx_Clock)    (injected frame, and
                                                        with --signal_bus the
                                                        Tx_Data of the other
                                                        Bus Terminals)
    fmi3UpdateDiscreteStates
    fmi3EnterStepMode
*/


extern bool signal_bus_enabled;


int lsbus_load(LsBusFmi* api, void* handle, void* fmu)
{
    *api = (LsBusFmi){
        .fmu = fmu,
        .get_binary = dlsym(handle, "fmi3GetBinary"),
        .set_binary = dlsym(handle, "fmi3SetBinary"),
        .set_clock = dlsym(handle, "fmi3SetClock"),
        .enter_event_mode = dlsym(handle, "fmi3EnterEventMode"),
        .update_discrete_states = dlsym(handle, "fmi3UpdateDiscreteStates"),
        .enter_step_mode = dlsym(handle, "fmi3EnterStepMode"),
    };
    if (api->get_binary == NULL || api->set_binary == NULL ||
        api->set_clock == NULL || api->enter_event_mode == NULL ||
        api->update_discrete_states == NULL || api->enter_step_mode == NULL) {
        return EINVAL;
    }
    return 0;
}


static NCODEC* _open_codec(const char* mime_type)
{
    NCODEC* nc = lsbus_codec_create(mime_type);
    if (nc) ((NCodecInstance*)nc)->stream = ncodec_buffer_stream_create(1024);
    return nc;
}


static void _print_frames(LsBusTerminal* t, const uint8_t* data, size_t len)
{
    NCODEC* nc = _open_codec(t->mime_type);
    if (nc == NULL) return;
    ((NCodecInstance*)nc)->stream->write(nc, (uint8_t*)data, len);
    ncodec_seek(nc, 0, NCODEC_SEEK_SET);
    NCodecCanMessage msg;
    while (ncodec_read(nc, &msg) >= 0) {
        printf("Importer: LS-BUS %s Tx_Data: id=%u len=%zu\n", t->name,
            msg.frame_id, msg.len);
        printf("Importer: network message (RX): %.*s\n", (int)msg.len,
            msg.buffer ? (char*)msg.buffer : "");
    }
    fflush(stdout);
    ncodec_close(nc);
}


int lsbus_event_mode(
    modelDescription* desc, LsBusFmi* api, size_t step, bool enter)
{
    size_t    count = desc->lsbus.count;
    uint8_t** tx_data = calloc(count, sizeof(uint8_t*));
    size_t*   tx_len = calloc(count, sizeof(size_t));
    bool      active = true;
    int       rc = 0;

    if (enter) rc |= api->enter_event_mode(api->fmu);

    /* Tx_Clock ticks: read the Tx_Data of all Bus Terminals. */
    for (size_t i = 0; i < count; i++) {
        LsBusTerminal* t = &desc->lsbus.terminal[i];
        if (t->tx == false) continue;
        const uint8_t* value = NULL;
        rc |= api->set_clock(api->fmu, &t->tx_clock_vr, 1, &active);
        rc |= api->get_binary(api->fmu, &t->tx_vr, 1, &tx_len[i], &value, 1);
        if (value == NULL || tx_len[i] == 0) continue;
        /* Copy, the FMU may release the value on the next call. */
        tx_data[i] = malloc(tx_len[i]);
        memcpy(tx_data[i], value, tx_len[i]);
        _print_frames(t, tx_data[i], tx_len[i]);
    }

    /* Rx: an injected CAN frame, plus the other Bus Terminals (ideal bus). */
    for (size_t i = 0; i < count; i++) {
        LsBusTerminal* t = &desc->lsbus.terminal[i];
        if (t->rx == false) continue;
        NCODEC* nc = _open_codec(t->mime_type);
        if (nc == NULL) {
            printf("Importer: LS-BUS %s: unsupported mimeType %s\n", t->name,
                t->mime_type);
            continue;
        }
        char inject[64];
        snprintf(inject, sizeof(inject), "Hello from Importer (%zu)", step + 1);
        ncodec_write(nc, &(NCodecCanMessage){ .frame_id = 42 + step * 10,
                             .frame_type = CAN_FD_BASE_FRAME,
                             .buffer = (uint8_t*)inject,
                             .len = strlen(inject) + 1 });
        ncodec_flush(nc);
        for (size_t j = 0; signal_bus_enabled && j < count; j++) {
            if (j == i || tx_data[j] == NULL) continue;
            ncodec_seek(nc, 0, NCODEC_SEEK_END);
            ((NCodecInstance*)nc)->stream->write(nc, tx_data[j], tx_len[j]);
        }
        uint8_t* data = NULL;
        size_t   len = 0;
        ncodec_seek(nc, 0, NCODEC_SEEK_SET);
        ((NCodecInstance*)nc)->stream->read(nc, &data, &len, NCODEC_POS_NC);
        if (len) {
            rc |= api->set_binary(api->fmu, &t->rx_vr, 1, &len, &data, 1);
            rc |= api->set_clock(api->fmu, &t->rx_clock_vr, 1, &active);
        }
        ncodec_close(nc);
    }

    bool need_update, terminate, nominals_changed, values_changed, next_defined;
    double next_time;
    rc |= api->update_discrete_states(api->fmu, &need_update, &terminate,
        &nominals_changed, &values_changed, &next_defined, &next_time);
    rc |= api->enter_step_mode(api->fmu);

    for (size_t i = 0; i < count; i++) {
        free(tx_data[i]);
    }
    free(tx_data);
    free(tx_len);
    return rc;
}


void lsbus_free(modelDescription* desc)
{
    for (size_t i = 0; i < desc->lsbus.count; i++) {
        free(desc->lsbus.terminal[i].name);
        free(desc->lsbus.terminal[i].mime_type);
    }
    free(desc->lsbus.terminal);
}
