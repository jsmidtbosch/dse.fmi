// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <string.h>
#include <libxml/tree.h>
#include <dse/modelc/runtime.h>
#include <dse/ncodec/codec.h>
#include <dse/ncodec/interface/frame.h>
#include <dse/ncodec/interface/pdu.h>
#include <dse/ncodec_lsbus/codec.h>
#include <dse/fmu/fmu.h>
#include <dse/fmigateway/fmigateway.h>


#define LSBUS_ENABLED_ANNOTATION "dse.standards.fmi-ls-bus.enabled"
#define LSBUS_CAN_MIME_TYPE                                                    \
    "application/org.fmi-standard.fmi-ls-bus.can; version=\"1.0.0\""

enum {
    LSBUS_RX_VARIABLE_INDEX = 0,
    LSBUS_TX_VARIABLE_INDEX,
    LSBUS_VARIABLE_COUNT,
};

typedef enum FmiGatewayLsBusType {
    FMIGATEWAY_LSBUS_TYPE_NONE,
    FMIGATEWAY_LSBUS_TYPE_CAN_FRAME,
    FMIGATEWAY_LSBUS_TYPE_CAN_PDU,
} FmiGatewayLsBusType;


/* One FMI-LS-BUS Bus Terminal, bridged to a SimBus CAN frame/PDU signal. */
typedef struct FmiGatewayLsBus {
    /* Storage of the FMI variables: Rx_Data, then Tx_Data. */
    FmuSignalVector     fmu_sv;
    char*               signal_names[LSBUS_VARIABLE_COUNT];
    void*               binary_buffers[LSBUS_VARIABLE_COUNT];
    uint32_t            signal_lengths[LSBUS_VARIABLE_COUNT];
    uint32_t            buffer_sizes[LSBUS_VARIABLE_COUNT];
    NCODEC*             rx_codec;
    NCODEC*             tx_codec;
    /* Owned by ModelC. */
    NCODEC*             simbus_codec;
    FmiGatewayLsBusType simbus_type;
} FmiGatewayLsBus;


extern void fmu_variable_index_clocks(xmlDoc* doc, FmuInstanceData* fmu);


static bool __stat_is(NCODEC* codec, const char* name, const char* value)
{
    for (int32_t i = 0; i >= 0; i++) {
        NCodecConfigItem config_item = ncodec_stat(codec, &i);
        if (i < 0) break;
        if (config_item.name && config_item.value &&
            strcmp(config_item.name, name) == 0) {
            return strcmp(config_item.value, value) == 0;
        }
    }
    return false;
}


/* NCodecCanFrameType and NCodecPduCanFrameFormat have the same values. */
static void __bus_write(FmiGatewayLsBus* terminal, NCodecCanMessage* message)
{
    if (terminal->simbus_type == FMIGATEWAY_LSBUS_TYPE_CAN_FRAME) {
        ncodec_write(terminal->simbus_codec, message);
        return;
    }
    NCodecPdu can_pdu = {
        .id = message->frame_id,
        .payload = message->buffer,
        .payload_len = message->len,
        .transport_type = NCodecPduTransportTypeCan,
        .transport.can_message.frame_format =
            (NCodecPduCanFrameFormat)message->frame_type,
    };
    ncodec_write(terminal->simbus_codec, &can_pdu);
}


static int32_t __bus_read(FmiGatewayLsBus* terminal, NCodecCanMessage* message)
{
    if (terminal->simbus_type == FMIGATEWAY_LSBUS_TYPE_CAN_FRAME) {
        return ncodec_read(terminal->simbus_codec, message);
    }

    NCodecPdu can_pdu;
    int32_t   rc;
    while ((rc = ncodec_read(terminal->simbus_codec, &can_pdu)) >= 0) {
        if (can_pdu.transport_type != NCodecPduTransportTypeCan) continue;
        if (can_pdu.transport.can_message.frame_type !=
            NCodecPduCanFrameTypeData) {
            continue;
        }
        *message = (NCodecCanMessage){
            .frame_id = can_pdu.id,
            .frame_type =
                (NCodecCanFrameType)can_pdu.transport.can_message.frame_format,
            .buffer = (uint8_t*)can_pdu.payload,
            .len = can_pdu.payload_len,
        };
        return rc;
    }
    return rc;
}


static NCODEC* __open(FmuSignalVector* fmu_sv, uint32_t variable_index)
{
    NCODEC* codec = lsbus_codec_create(LSBUS_CAN_MIME_TYPE);
    ((NCodecInstance*)codec)->stream =
        fmu_sv_stream_create(fmu_sv, variable_index);
    return codec;
}


static void __setup_signal(FmuInstanceData* fmu,
    ModelGatewayDesc* model_gateway, SignalVector* sv, uint32_t vector_index)
{
    FmiGateway*      fmi_gateway = fmu->data;
    ModelSignalIndex model_signal =
        signal_index((ModelDesc*)model_gateway->mi->model_desc, sv->alias,
            sv->signal[vector_index]);
    if (model_signal.sv == NULL) return;

    NCODEC* simbus_codec = signal_codec(model_signal.sv, model_signal.signal);
    if (simbus_codec == NULL) return;

    bool is_can_frame = __stat_is(simbus_codec, "type", "frame") &&
                        __stat_is(simbus_codec, "bus", "can");
    bool is_can_pdu = __stat_is(simbus_codec, "type", "pdu") &&
                      __stat_is(simbus_codec, "model", "can");

    FmiGatewayLsBusType simbus_type = FMIGATEWAY_LSBUS_TYPE_NONE;
    if (is_can_frame) simbus_type = FMIGATEWAY_LSBUS_TYPE_CAN_FRAME;
    if (is_can_pdu) simbus_type = FMIGATEWAY_LSBUS_TYPE_CAN_PDU;

    /* Ensure that the signal is either a CAN frame or a CAN PDU. */
    if (simbus_type == FMIGATEWAY_LSBUS_TYPE_NONE) {
        fmu_log(fmu, FmiLogError, "Error",
            "LS-BUS: signal %s has no CAN frame or CAN PDU mime_type",
            sv->signal[vector_index]);
        return;
    }

    const char** rx_vrefs = _signal_annotation_list(sv->mi, sv,
        sv->signal[vector_index], "dse.standards.fmi-ls-bus-topology.rx_vref");
    const char** tx_vrefs = _signal_annotation_list(sv->mi, sv,
        sv->signal[vector_index], "dse.standards.fmi-ls-bus-topology.tx_vref");
    for (size_t vref_index = 0;
        rx_vrefs && tx_vrefs && rx_vrefs[vref_index] && tx_vrefs[vref_index];
        vref_index++) {
        FmuSignalVectorIndex* rx_variable =
            fmu_variable_find(&fmu->variables.binary.rx,
                (uint32_t)strtoul(rx_vrefs[vref_index], NULL, 10));
        FmuSignalVectorIndex* tx_variable =
            fmu_variable_find(&fmu->variables.binary.tx,
                (uint32_t)strtoul(tx_vrefs[vref_index], NULL, 10));
        if (rx_variable == NULL || tx_variable == NULL) continue;

        FmiGatewayLsBus* terminal = calloc(1, sizeof(FmiGatewayLsBus));
        terminal->signal_names[LSBUS_RX_VARIABLE_INDEX] =
            (char*)sv->signal[vector_index];
        terminal->signal_names[LSBUS_TX_VARIABLE_INDEX] =
            (char*)sv->signal[vector_index];

        terminal->fmu_sv = (FmuSignalVector){
            .count = LSBUS_VARIABLE_COUNT,
            .signal = terminal->signal_names,
            .binary = terminal->binary_buffers,
            .length = terminal->signal_lengths,
            .buffer_size = terminal->buffer_sizes,
        };

        /* Open the LS-BUS codecs for the RX and TX variables. */
        terminal->rx_codec = __open(&terminal->fmu_sv, LSBUS_RX_VARIABLE_INDEX);
        terminal->tx_codec = __open(&terminal->fmu_sv, LSBUS_TX_VARIABLE_INDEX);
        lsbus_codec_link(terminal->rx_codec, terminal->tx_codec);
        terminal->simbus_codec = simbus_codec;
        terminal->simbus_type = simbus_type;

        /* The FMI variables now carry LS-BUS operations, not the SimBus. */
        *rx_variable->sv = terminal->fmu_sv;
        rx_variable->vi = LSBUS_RX_VARIABLE_INDEX;
        *tx_variable->sv = terminal->fmu_sv;
        tx_variable->vi = LSBUS_TX_VARIABLE_INDEX;
        vector_push(&fmi_gateway->lsbus, &terminal);
    }
    free(rx_vrefs);
    free(tx_vrefs);
}


bool fmigateway_lsbus_enabled(SignalVector* sv, uint32_t i)
{
    const char* enabled_value =
        signal_annotation(sv, i, LSBUS_ENABLED_ANNOTATION, NULL);
    return enabled_value && strcmp(enabled_value, "true") == 0;
}


void fmigateway_lsbus_index_clocks(FmuInstanceData* fmu, xmlDoc* xml_document)
{
    fmu_variable_index_clocks(xml_document, fmu);
}


void fmigateway_lsbus_setup(
    FmuInstanceData* fmu, ModelGatewayDesc* model_gateway)
{
    FmiGateway* fmi_gateway = fmu->data;
    fmi_gateway->lsbus = vector_make(sizeof(FmiGatewayLsBus*), 0, NULL);

    for (ModelInstanceSpec* instance = model_gateway->sim->instance_list;
        instance && instance->name; instance++) {
        for (SignalVector* sv = instance->model_desc->sv; sv && sv->name;
            sv++) {
            if (sv->is_binary == false) continue;
            for (uint32_t i = 0; i < sv->count; i++) {
                if (fmigateway_lsbus_enabled(sv, i) == false) {
                    continue;
                }
                __setup_signal(fmu, model_gateway, sv, i);
            }
        }
    }

    fmu_log(fmu, 0, "Debug", "  LS-BUS: terminals=%u",
        (uint32_t)vector_len(&fmi_gateway->lsbus));
}


static FmiGatewayLsBus* __terminal(FmiGateway* fmi_gateway, size_t i)
{
    return *(FmiGatewayLsBus**)vector_at(&fmi_gateway->lsbus, i, NULL);
}


/* Before the SimBus step: Rx_Data (from the importer) -> SimBus. */
void fmigateway_lsbus_rx(FmuInstanceData* fmu)
{
    FmiGateway* fmi_gateway = fmu->data;
    size_t      terminal_count = vector_len(&fmi_gateway->lsbus);

    /* Truncate (also resets the reader) before any Bus Terminal writes. Keep
       the SimBus content if the previous step was not sent (gateway behind).
       Tx_Data of the previous step was read by the importer. */
    for (size_t i = 0; i < terminal_count; i++) {
        FmiGatewayLsBus* terminal = __terminal(fmi_gateway, i);
        if (fmi_gateway->lsbus_pending == false) {
            ncodec_truncate(terminal->simbus_codec);
        }
        ncodec_truncate(terminal->tx_codec);
    }
    for (size_t i = 0; i < terminal_count; i++) {
        FmiGatewayLsBus* terminal = __terminal(fmi_gateway, i);
        NCodecCanMessage message;

        ncodec_seek(terminal->simbus_codec, 0, NCODEC_SEEK_END);
        ncodec_seek(terminal->rx_codec, 0, NCODEC_SEEK_SET);
        while (ncodec_read(terminal->rx_codec, &message) >= 0) {
            __bus_write(terminal, &message);
            /* SimBus does not return a node its own frames, so forward to
               the other Bus Terminals of this SimBus signal directly. */
            for (size_t peer_index = 0; peer_index < terminal_count;
                peer_index++) {
                FmiGatewayLsBus* peer_terminal =
                    __terminal(fmi_gateway, peer_index);
                if (peer_index != i &&
                    peer_terminal->simbus_codec == terminal->simbus_codec) {
                    ncodec_write(peer_terminal->tx_codec, &message);
                }
            }
        }
        ncodec_truncate(terminal->rx_codec);
        ncodec_flush(terminal->simbus_codec);
    }
}


/* After the SimBus step: SimBus -> Tx_Data (to the importer). If the step
   was not sent (gateway behind) the SimBus is not read. */
void fmigateway_lsbus_tx(FmuInstanceData* fmu, bool step_synced)
{
    FmiGateway* fmi_gateway = fmu->data;
    size_t      terminal_count = vector_len(&fmi_gateway->lsbus);

    fmi_gateway->lsbus_pending = !step_synced;
    for (size_t i = 0; step_synced && i < terminal_count; i++) {
        FmiGatewayLsBus* terminal = __terminal(fmi_gateway, i);
        NCodecCanMessage message;

        /* Each SimBus signal is read once, for all its Bus Terminals. */
        bool simbus_signal_already_read = false;
        for (size_t j = 0; j < i; j++) {
            simbus_signal_already_read |=
                (__terminal(fmi_gateway, j)->simbus_codec ==
                    terminal->simbus_codec);
        }
        if (simbus_signal_already_read) continue;

        ncodec_seek(terminal->simbus_codec, 0, NCODEC_SEEK_SET);
        while (__bus_read(terminal, &message) >= 0) {
            for (size_t destination_index = i;
                destination_index < terminal_count; destination_index++) {
                FmiGatewayLsBus* destination_terminal =
                    __terminal(fmi_gateway, destination_index);
                if (destination_terminal->simbus_codec !=
                    terminal->simbus_codec) {
                    continue;
                }
                if (ncodec_write(destination_terminal->tx_codec, &message) <
                    0) {
                    fmu_log(fmu, FmiLogWarning, "Warning",
                        "LS-BUS: frame id=%u len=%zu dropped (invalid CAN "
                        "data length for frame type %d)",
                        message.frame_id, message.len, message.frame_type);
                }
            }
        }
    }
    for (size_t i = 0; i < terminal_count; i++) {
        ncodec_flush(__terminal(fmi_gateway, i)->tx_codec);
    }
}


void fmigateway_lsbus_destroy(FmuInstanceData* fmu)
{
    FmiGateway* fmi_gateway = fmu->data;
    for (size_t i = 0; i < vector_len(&fmi_gateway->lsbus); i++) {
        FmiGatewayLsBus* terminal = __terminal(fmi_gateway, i);
        ncodec_close(terminal->rx_codec);
        ncodec_close(terminal->tx_codec);
        free(terminal->binary_buffers[LSBUS_RX_VARIABLE_INDEX]);
        free(terminal->binary_buffers[LSBUS_TX_VARIABLE_INDEX]);
        free(terminal);
    }
    vector_reset(&fmi_gateway->lsbus);
}
