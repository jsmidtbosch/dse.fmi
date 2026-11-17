// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <stdlib.h>
#include <libxml/xpath.h>
#include <dse/fmu/fmu.h>


#define FMI3_CLOCK_XPATH                                                       \
    "/fmiModelDescription/ModelVariables/Clock[@intervalDecimal]"


void fmu_variable_index_clocks(xmlDoc* doc, FmuInstanceData* fmu)
{
    xmlXPathContext* ctx = xmlXPathNewContext(doc);
    xmlXPathObject*  obj =
        xmlXPathEvalExpression((xmlChar*)FMI3_CLOCK_XPATH, ctx);
    xmlXPathFreeContext(ctx);
    if (obj == NULL || obj->nodesetval == NULL) {
        xmlXPathFreeObject(obj);
        return;
    }

    for (int i = 0; i < obj->nodesetval->nodeNr; i++) {
        xmlNode* node = obj->nodesetval->nodeTab[i];
        xmlChar* vr = xmlGetProp(node, (xmlChar*)"valueReference");
        xmlChar* interval = xmlGetProp(node, (xmlChar*)"intervalDecimal");
        xmlChar* shift = xmlGetProp(node, (xmlChar*)"shiftDecimal");
        if (vr) {
            FmuClock* clock = calloc(1, sizeof(FmuClock));
            clock->interval = strtod((char*)interval, NULL);
            clock->shift = shift ? strtod((char*)shift, NULL) : 0.0;
            fmu_variable_add(&fmu->variables.clock,
                (uint32_t)strtoul((char*)vr, NULL, 10), clock);
        }
        xmlFree(vr);
        xmlFree(interval);
        xmlFree(shift);
    }
    xmlXPathFreeObject(obj);
    vector_sort(&fmu->variables.clock);
}
