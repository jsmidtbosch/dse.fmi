// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dse/ncodec/codec.h>
#include <dse/ncodec/interface/frame.h>
#include <dse/ncodec_lsbus/codec.h>
#include "fmi3LsBusCan.h"


#define CODEC_NAME    "application/org.fmi-standard.fmi-ls-bus.can"
#define CODEC_VERSION "1.0.0"


typedef struct LsBusCodecInstance {
    NCodecInstance             c;
    char*                      bus_id;
    char*                      node_id;
    char*                      interface_id;
    /* Operations written but not yet flushed to the stream. */
    uint8_t*                   tx;
    size_t                     tx_len;
    size_t                     tx_size;
    /* Tx codec of the same Bus Terminal, receives Format Error operations. */
    struct LsBusCodecInstance* peer;
} LsBusCodecInstance;


static void log_trace(LsBusCodecInstance* nc, const char* format, ...)
{
    if (nc->c.trace.log == NULL) return;
    char    msg[256];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    nc->c.trace.log((NCODEC*)nc, NCODEC_LOG_TRACE, msg);
}


static void tx_append(LsBusCodecInstance* nc, const void* data, size_t len)
{
    if (nc->tx_len + len > nc->tx_size) {
        nc->tx_size = (nc->tx_len + len) * 2;
        nc->tx = realloc(nc->tx, nc->tx_size);
    }
    memcpy(nc->tx + nc->tx_len, data, len);
    nc->tx_len += len;
}


/* Smallest valid data length >= len, or -1 (CAN max 8, CAN FD max 64). */
static int can_dl(bool fd, size_t len)
{
    static const int fd_dl[] = { 12, 16, 20, 24, 32, 48, 64 };
    if (len <= 8) return (int)len;
    for (size_t i = 0; fd && i < sizeof(fd_dl) / sizeof(fd_dl[0]); i++) {
        if ((int)len <= fd_dl[i]) return fd_dl[i];
    }
    return -1;
}


static bool op_valid(const uint8_t* data, uint32_t len)
{
    const fmi3LsBusOperationHeader* hdr = (const void*)data;
    switch (hdr->opCode) {
    case FMI3_LS_BUS_CAN_OP_CAN_TRANSMIT: {
        const fmi3LsBusCanOperationCanTransmit* op = (const void*)data;
        if (len < sizeof(*op) || op->dataLength > 8) return false;
        /* RTR: no Data, DL is the requested length. */
        if (op->rtr) return len == sizeof(*op);
        return len == sizeof(*op) + op->dataLength;
    }
    case FMI3_LS_BUS_CAN_OP_CANFD_TRANSMIT: {
        const fmi3LsBusCanOperationCanFdTransmit* op = (const void*)data;
        return len >= sizeof(*op) && len == sizeof(*op) + op->dataLength &&
               can_dl(true, op->dataLength) == op->dataLength;
    }
    case FMI3_LS_BUS_CAN_OP_CANXL_TRANSMIT: {
        const fmi3LsBusCanOperationCanXlTransmit* op = (const void*)data;
        return len >= sizeof(*op) && len == sizeof(*op) + op->dataLength;
    }
    case FMI3_LS_BUS_CAN_OP_CONFIRM:
    case FMI3_LS_BUS_CAN_OP_ARBITRATION_LOST:
        return len == sizeof(fmi3LsBusCanOperationConfirm);
    case FMI3_LS_BUS_CAN_OP_BUS_ERROR:
        return len == sizeof(fmi3LsBusCanOperationBusError);
    case FMI3_LS_BUS_CAN_OP_CONFIGURATION:
        return len >= sizeof(fmi3LsBusOperationHeader) + 1;
    case FMI3_LS_BUS_CAN_OP_STATUS:
        return len == sizeof(fmi3LsBusCanOperationStatus);
    case FMI3_LS_BUS_CAN_OP_WAKEUP:
        return len == sizeof(fmi3LsBusCanOperationWakeup);
    default:
        return false;
    }
}


static void format_error(
    LsBusCodecInstance* nc, const uint8_t* data, size_t len)
{
    log_trace(nc, "LS-BUS: format error (%zu bytes)", len);
    LsBusCodecInstance* peer = nc->peer;
    if (peer == NULL || peer->c.stream == NULL) return;

    if (len > UINT16_MAX) len = UINT16_MAX;
    fmi3LsBusOperationFormatError op = {
        .header = { FMI3_LS_BUS_OP_FORMAT_ERROR, sizeof(op) + len },
        .dataLength = len,
    };
    peer->c.stream->write((NCODEC*)peer, (uint8_t*)&op, sizeof(op));
    peer->c.stream->write((NCODEC*)peer, (uint8_t*)data, len);
}


static int32_t codec_config(NCODEC* nc, NCodecConfigItem item)
{
    LsBusCodecInstance* _nc = (LsBusCodecInstance*)nc;
    if (_nc == NULL) return -ENOSTR;

    char** field = NULL;
    if (strcmp(item.name, "bus_id") == 0) field = &_nc->bus_id;
    if (strcmp(item.name, "node_id") == 0) field = &_nc->node_id;
    if (strcmp(item.name, "interface_id") == 0) field = &_nc->interface_id;
    if (field == NULL) return -EINVAL;

    free(*field);
    *field = strdup(item.value);
    return 0;
}


static NCodecConfigItem codec_stat(NCODEC* nc, int32_t* index)
{
    LsBusCodecInstance* _nc = (LsBusCodecInstance*)nc;
    if (_nc == NULL || index == NULL) return (NCodecConfigItem){};

    switch (*index) {
    case 0:
        return (NCodecConfigItem){ "type", "frame" };
    case 1:
        return (NCodecConfigItem){ "bus", "can" };
    case 2:
        return (NCodecConfigItem){ "version", CODEC_VERSION };
    case 3:
        return (NCodecConfigItem){ "bus_id", _nc->bus_id };
    case 4:
        return (NCodecConfigItem){ "node_id", _nc->node_id };
    case 5:
        return (NCodecConfigItem){ "interface_id", _nc->interface_id };
    default:
        *index = -1;
        return (NCodecConfigItem){};
    }
}


static int32_t codec_write(NCODEC* nc, NCodecMessage* msg)
{
    LsBusCodecInstance* _nc = (LsBusCodecInstance*)nc;
    NCodecCanMessage*   _msg = (NCodecCanMessage*)msg;
    if (_nc == NULL) return -ENOSTR;
    if (_msg == NULL) return -EINVAL;
    if (_nc->c.stream == NULL) return -ENOSR;

    bool fd = (_msg->frame_type == CAN_FD_BASE_FRAME ||
               _msg->frame_type == CAN_FD_EXTENDED_FRAME);
    int  dl = can_dl(fd, _msg->len);
    if (dl < 0) return -EINVAL;

    fmi3LsBusCanIde ide = (_msg->frame_type == CAN_EXTENDED_FRAME ||
                              _msg->frame_type == CAN_FD_EXTENDED_FRAME)
                              ? FMI3_LS_BUS_TRUE
                              : FMI3_LS_BUS_FALSE;
    if (fd) {
        fmi3LsBusCanOperationCanFdTransmit op = {
            .header = { FMI3_LS_BUS_CAN_OP_CANFD_TRANSMIT, sizeof(op) + dl },
            .id = _msg->frame_id,
            .ide = ide,
            .brs = FMI3_LS_BUS_TRUE,
            .esi = FMI3_LS_BUS_FALSE,
            .dataLength = dl,
        };
        tx_append(_nc, &op, sizeof(op));
    } else {
        fmi3LsBusCanOperationCanTransmit op = {
            .header = { FMI3_LS_BUS_CAN_OP_CAN_TRANSMIT, sizeof(op) + dl },
            .id = _msg->frame_id,
            .ide = ide,
            .rtr = FMI3_LS_BUS_FALSE,
            .dataLength = dl,
        };
        tx_append(_nc, &op, sizeof(op));
    }
    if (_msg->len) tx_append(_nc, _msg->buffer, _msg->len);
    static const uint8_t padding[64];
    if ((size_t)dl > _msg->len) tx_append(_nc, padding, dl - _msg->len);

    return _msg->len;
}


static int32_t codec_read(NCODEC* nc, NCodecMessage* msg)
{
    LsBusCodecInstance* _nc = (LsBusCodecInstance*)nc;
    NCodecCanMessage*   _msg = (NCodecCanMessage*)msg;
    if (_nc == NULL) return -ENOSTR;
    if (_msg == NULL) return -EINVAL;
    if (_nc->c.stream == NULL) return -ENOSR;
    NCodecStreamVTable* stream = _nc->c.stream;

    *_msg = (NCodecCanMessage){};
    uint8_t* data = NULL;
    size_t   len = 0;
    stream->read(nc, &data, &len, NCODEC_POS_NC);
    while (data && len) {
        fmi3LsBusOperationHeader* hdr = (fmi3LsBusOperationHeader*)data;
        if (len < sizeof(*hdr) || hdr->length < sizeof(*hdr) ||
            hdr->length > len) {
            /* Operation boundaries are lost, report the rest and stop. */
            format_error(_nc, data, len);
            break;
        }
        stream->seek(nc, hdr->length, NCODEC_SEEK_CUR);

        if (hdr->opCode == FMI3_LS_BUS_OP_FORMAT_ERROR) {
            /* Never answered, to avoid Format Error loops. */
        } else if (!op_valid(data, hdr->length)) {
            format_error(_nc, data, hdr->length);
        } else if (hdr->opCode == FMI3_LS_BUS_CAN_OP_CAN_TRANSMIT) {
            fmi3LsBusCanOperationCanTransmit* op = (void*)data;
            _msg->frame_id = op->id;
            _msg->frame_type = op->ide ? CAN_EXTENDED_FRAME : CAN_BASE_FRAME;
            _msg->buffer = op->rtr ? NULL : op->data;
            _msg->len = op->rtr ? 0 : op->dataLength;
            return _msg->len;
        } else if (hdr->opCode == FMI3_LS_BUS_CAN_OP_CANFD_TRANSMIT) {
            fmi3LsBusCanOperationCanFdTransmit* op = (void*)data;
            _msg->frame_id = op->id;
            _msg->frame_type =
                op->ide ? CAN_FD_EXTENDED_FRAME : CAN_FD_BASE_FRAME;
            _msg->buffer = op->data;
            _msg->len = op->dataLength;
            return _msg->len;
        } else {
            /* BusNotifications=false: ignore notifications, CAN XL ... */
            log_trace(_nc, "LS-BUS: skip operation 0x%x", hdr->opCode);
        }
        stream->read(nc, &data, &len, NCODEC_POS_NC);
    }

    stream->seek(nc, 0, NCODEC_SEEK_END);
    return -ENOMSG;
}


static int32_t codec_flush(NCODEC* nc)
{
    LsBusCodecInstance* _nc = (LsBusCodecInstance*)nc;
    if (_nc == NULL) return -ENOSTR;
    if (_nc->c.stream == NULL) return -ENOSR;

    int32_t len = _nc->tx_len;
    if (len) _nc->c.stream->write(nc, _nc->tx, _nc->tx_len);
    _nc->tx_len = 0;
    return len;
}


static int32_t codec_truncate(NCODEC* nc)
{
    LsBusCodecInstance* _nc = (LsBusCodecInstance*)nc;
    if (_nc == NULL) return -ENOSTR;
    if (_nc->c.stream == NULL) return -ENOSR;

    _nc->tx_len = 0;
    _nc->c.stream->seek(nc, 0, NCODEC_SEEK_RESET);
    return 0;
}


static void codec_close(NCODEC* nc)
{
    LsBusCodecInstance* _nc = (LsBusCodecInstance*)nc;
    if (_nc == NULL) return;

    free(_nc->bus_id);
    free(_nc->node_id);
    free(_nc->interface_id);
    free(_nc->tx);
    free(_nc);
}


static char* unquote(char* s)
{
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        s++;
    }
    return s;
}


NCODEC* lsbus_codec_create(const char* mime_type)
{
    if (mime_type == NULL) return NULL;

    char* buf = strdup(mime_type);
    char* pos = NULL;
    char* name = strtok_r(buf, "; ", &pos);
    if (name == NULL || strcmp(name, CODEC_NAME) != 0) {
        free(buf);
        return NULL;
    }

    LsBusCodecInstance* nc = calloc(1, sizeof(LsBusCodecInstance));
    nc->c.mime_type = mime_type;
    nc->c.codec = (NCodecVTable){
        .config = codec_config,
        .stat = codec_stat,
        .write = codec_write,
        .read = codec_read,
        .flush = codec_flush,
        .truncate = codec_truncate,
        .close = codec_close,
    };
    nc->bus_id = strdup("0");
    nc->node_id = strdup("0");
    nc->interface_id = strdup("0");

    bool  version_ok = false;
    char* param;
    while ((param = strtok_r(NULL, "; ", &pos)) != NULL) {
        char* value = strchr(param, '=');
        if (value == NULL) continue;
        *value++ = '\0';
        value = unquote(value);
        if (strcmp(param, "version") == 0) {
            version_ok = (strcmp(value, CODEC_VERSION) == 0);
        } else {
            codec_config((NCODEC*)nc, (NCodecConfigItem){ param, value });
        }
    }
    free(buf);

    if (!version_ok) {
        codec_close((NCODEC*)nc);
        return NULL;
    }
    return (NCODEC*)nc;
}


void lsbus_codec_link(NCODEC* rx, NCODEC* tx)
{
    LsBusCodecInstance* _rx = (LsBusCodecInstance*)rx;
    LsBusCodecInstance* _tx = (LsBusCodecInstance*)tx;
    if (_rx == NULL || _tx == NULL) return;
    if (_rx->c.codec.read != codec_read || _tx->c.codec.read != codec_read) {
        return;
    }
    _rx->peer = _tx;
}
