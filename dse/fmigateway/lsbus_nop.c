// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <dse/fmigateway/fmigateway.h>

bool fmigateway_lsbus_enabled(SignalVector* sv, uint32_t index)
{
    UNUSED(sv);
    UNUSED(index);
    return false;
}

void fmigateway_lsbus_index_clocks(FmuInstanceData* fmu, struct _xmlDoc* doc)
{
    UNUSED(fmu);
    UNUSED(doc);
}

void fmigateway_lsbus_setup(FmuInstanceData* fmu, ModelGatewayDesc* m)
{
    UNUSED(fmu);
    UNUSED(m);
}

void fmigateway_lsbus_rx(FmuInstanceData* fmu)
{
    UNUSED(fmu);
}

void fmigateway_lsbus_tx(FmuInstanceData* fmu, bool synced)
{
    UNUSED(fmu);
    UNUSED(synced);
}

void fmigateway_lsbus_destroy(FmuInstanceData* fmu)
{
    UNUSED(fmu);
}
