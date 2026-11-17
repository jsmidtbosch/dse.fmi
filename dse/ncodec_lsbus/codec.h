// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#ifndef DSE_NCODEC_LSBUS_CODEC_H_
#define DSE_NCODEC_LSBUS_CODEC_H_

#include <dse/ncodec/codec.h>


/**
FMI-LS-BUS CAN Codec
====================

Network Codec for the FMI-LS-BUS 1.0 CAN Low-Cut wire format. Reads and
writes `NCodecCanMessage` objects as CAN Transmit (0x10) and CAN FD
Transmit (0x11) operations.

MIME type:

    application/org.fmi-standard.fmi-ls-bus.can; version="1.0.0"

Optional parameters: `bus_id`, `node_id`, `interface_id` (default 0).

Returns NULL if the MIME type is not an FMI-LS-BUS CAN MIME type.
*/
NCODEC* lsbus_codec_create(const char* mime_type);

/* Format Error operations for corrupt `rx` operations are written to `tx`.
   Ignored unless both are LS-BUS codecs. */
void lsbus_codec_link(NCODEC* rx, NCODEC* tx);


#endif  // DSE_NCODEC_LSBUS_CODEC_H_
