// Copyright 2025 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <libxml/xpath.h>
#include <dse/clib/collections/hashlist.h>
#include <dse/ncodec_lsbus/codec.h>
#include <dse/fmu/fmu.h>


#define FMI3_SCALAR_XPATH "/fmiModelDescription/ModelVariables/Float64"
#define FMI3_BINARY_XPATH "/fmiModelDescription/ModelVariables/Binary"
#define FMI3_CODEC_ANNO_XPATH                                                  \
    "Annotations/Annotation[@type='dse.standards.fmi-ls-binary-codec']"        \
    "/Mimetype"


static xmlXPathObject* __eval(xmlDoc* doc, xmlNode* node, const char* xpath)
{
    xmlXPathContext* ctx = xmlXPathNewContext(doc);
    if (node) ctx->node = node;
    xmlXPathObject* obj = xmlXPathEvalExpression((xmlChar*)xpath, ctx);
    xmlXPathFreeContext(ctx);
    if (obj && obj->nodesetval && obj->nodesetval->nodeNr) return obj;
    xmlXPathFreeObject(obj);
    return NULL;
}


/* MIME type from the `mimeType` attribute, otherwise from the annotation. */
static char* __mime_type(xmlNode* node)
{
    char*    result = NULL;
    xmlChar* value = xmlGetProp(node, (xmlChar*)"mimeType");
    if (value == NULL) {
        xmlXPathObject* obj = __eval(node->doc, node, FMI3_CODEC_ANNO_XPATH);
        if (obj) {
            value = xmlNodeGetContent(obj->nodesetval->nodeTab[0]);
            xmlXPathFreeObject(obj);
        }
    }
    if (value) {
        result = strdup((char*)value);
        xmlFree(value);
    }
    return result;
}


/* Pair "<bus>Rx_Data" with "<bus>Tx_Data" (LS-BUS Bus Terminal naming). */
static void __link_lsbus(FmuSignalVector* sv)
{
    const size_t suffix = strlen("Rx_Data");
    for (uint32_t i = 0; i < sv->count; i++) {
        size_t len = strlen(sv->signal[i]);
        if (len < suffix || strcmp(sv->signal[i] + len - suffix, "Rx_Data"))
            continue;
        for (uint32_t j = 0; j < sv->count; j++) {
            if (strlen(sv->signal[j]) == len &&
                strncmp(sv->signal[j], sv->signal[i], len - suffix) == 0 &&
                strcmp(sv->signal[j] + len - suffix, "Tx_Data") == 0) {
                lsbus_codec_link(sv->ncodec[i], sv->ncodec[j]);
            }
        }
    }
}


size_t fmu_variable_count(xmlDoc* doc, bool is_binary)
{
    xmlXPathObject* obj =
        __eval(doc, NULL, is_binary ? FMI3_BINARY_XPATH : FMI3_SCALAR_XPATH);
    if (obj == NULL) return 0;
    size_t count = obj->nodesetval->nodeNr;
    xmlXPathFreeObject(obj);
    return count;
}


void fmu_variable_index(
    xmlDoc* doc, FmuInstanceData* fmu, FmuSignalVector* sv, bool is_binary)
{
    xmlXPathObject* obj =
        __eval(doc, NULL, is_binary ? FMI3_BINARY_XPATH : FMI3_SCALAR_XPATH);
    if (obj == NULL) return;

    for (int i = 0; i < obj->nodesetval->nodeNr; i++) {
        assert((uint32_t)i < sv->count);
        xmlNode* node = obj->nodesetval->nodeTab[i];
        xmlChar* name = xmlGetProp(node, (xmlChar*)"name");
        xmlChar* vr = xmlGetProp(node, (xmlChar*)"valueReference");
        xmlChar* causality = xmlGetProp(node, (xmlChar*)"causality");
        sv->signal[i] = strdup(name ? (char*)name : "");

        bool input = causality && xmlStrcmp(causality, (xmlChar*)"input") == 0;
        bool output =
            causality && xmlStrcmp(causality, (xmlChar*)"output") == 0;
        if (vr && (input || output)) {
            uint32_t vref = (uint32_t)strtoul((char*)vr, NULL, 10);
            if (is_binary) {
                FmuSignalVectorIndex* idx = calloc(1, sizeof(*idx));
                *idx = (FmuSignalVectorIndex){ .sv = sv, .vi = i };
                fmu_variable_add(input ? &fmu->variables.binary.rx
                                       : &fmu->variables.binary.tx,
                    vref, idx);
                sv->mime_type[i] = __mime_type(node);
                sv->ncodec[i] = fmu_ncodec_open(fmu, sv->mime_type[i], idx);
            } else {
                fmu_variable_add(input ? &fmu->variables.scalar.input
                                       : &fmu->variables.scalar.output,
                    vref, &sv->scalar[i]);
            }
        }

        xmlFree(name);
        xmlFree(vr);
        xmlFree(causality);
    }
    xmlXPathFreeObject(obj);

    if (is_binary) {
        __link_lsbus(sv);
        vector_sort(&fmu->variables.binary.rx);
        vector_sort(&fmu->variables.binary.tx);
    } else {
        vector_sort(&fmu->variables.scalar.input);
        vector_sort(&fmu->variables.scalar.output);
    }
}
