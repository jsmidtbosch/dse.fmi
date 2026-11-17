// Copyright 2026 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

package fmi3

import (
	"encoding/xml"
	"fmt"
	"os"
	"path/filepath"
	"strconv"

	schema_kind "github.com/boschglobal/dse.schemas/code/go/dse/kind"
)

// FMI-LS-BUS 1.0 (Network Abstraction, CAN) support.
const (
	LsBusEnabledAnnotation         = "dse.standards.fmi-ls-bus.enabled"
	LsBusRxClockAnnotation         = "dse.standards.fmi-ls-bus.rx_clock_vref"
	LsBusTxClockAnnotation         = "dse.standards.fmi-ls-bus.tx_clock_vref"
	LsBusTxClockIntervalAnnotation = "dse.standards.fmi-ls-bus.tx_clock_interval"
	LsBusCanMimeType               = `application/org.fmi-standard.fmi-ls-bus.can; version="1.0.0"`
)

const lsBusManifest = `<?xml version="1.0" encoding="UTF-8"?>
<fmiLayeredStandardManifest
    xmlns:fmi-ls="http://fmi-standard.org/fmi-ls-manifest"
    fmi-ls:fmi-ls-name="org.fmi-standard.fmi-ls-bus"
    fmi-ls:fmi-ls-version="1.0.0"
    fmi-ls:fmi-ls-description="Layered Standard for the simulation of bus communication on a Physical Signal Abstraction or Network Abstraction based level."
    isBusSimulationFMU="false"/>
`

// LsBusEnabled reports if the signal is annotated as an FMI-LS-BUS Bus Terminal.
func LsBusEnabled(signal schema_kind.Signal) bool {
	if signal.Annotations == nil {
		return false
	}
	switch v := (*signal.Annotations)[LsBusEnabledAnnotation].(type) {
	case bool:
		return v
	case string:
		return v == "true"
	}
	return false
}

// lsBusSignal adds one Bus Terminal (Rx_Data, Tx_Data, Rx_Clock, Tx_Clock)
// for each rx/tx value reference pair of the signal.
func lsBusSignal(md *ModelDescription, signal schema_kind.Signal, rx []int, tx []int) error {
	rxClock, okRx := toIntSlice((*signal.Annotations)[LsBusRxClockAnnotation])
	txClock, okTx := toIntSlice((*signal.Annotations)[LsBusTxClockAnnotation])
	interval, okInterval := (*signal.Annotations)[LsBusTxClockIntervalAnnotation].(string)
	if !okRx || !okTx || !okInterval || len(rxClock) != len(rx) || len(txClock) != len(tx) {
		return fmt.Errorf("LS-BUS signal %s: clock annotations missing or inconsistent", signal.Signal)
	}
	if md.ModelVariables.Clock == nil {
		md.ModelVariables.Clock = &[]Clock{}
	}
	if md.ModelStructure.Output == nil {
		md.ModelStructure.Output = &[]Output{}
	}

	discrete := "discrete"
	calculated := "calculated"
	mimeType := LsBusCanMimeType
	for x := range rx {
		name := signal.Signal
		if len(rx) > 1 {
			name = fmt.Sprintf("%s_%d", signal.Signal, x+1)
		}
		rxClk := strconv.Itoa(rxClock[x])
		txClk := strconv.Itoa(txClock[x])
		*md.ModelVariables.Binary = append(*md.ModelVariables.Binary,
			Binary{
				Name:           name + ".Rx_Data",
				ValueReference: strconv.Itoa(rx[x]),
				Causality:      "input",
				Variability:    &discrete,
				Clocks:         &rxClk,
				MimeType:       &mimeType,
				Start:          &Start{},
			},
			Binary{
				Name:           name + ".Tx_Data",
				ValueReference: strconv.Itoa(tx[x]),
				Causality:      "output",
				Variability:    &discrete,
				Clocks:         &txClk,
				Initial:        &calculated,
				MimeType:       &mimeType,
			},
		)
		*md.ModelVariables.Clock = append(*md.ModelVariables.Clock,
			Clock{
				Name:                name + ".Rx_Clock",
				ValueReference:      rxClk,
				Causality:           "input",
				Variability:         discrete,
				IntervalVariability: "triggered",
			},
			Clock{
				Name:                name + ".Tx_Clock",
				ValueReference:      txClk,
				Causality:           "input",
				Variability:         discrete,
				IntervalVariability: "constant",
				IntervalDecimal:     &interval,
			},
		)
		// Clocked output: listed as <Output>, not as <InitialUnknown>.
		*md.ModelStructure.Output = append(*md.ModelStructure.Output, Output{ValueReference: strconv.Itoa(tx[x])})
		md.LsBusTerminals = append(md.LsBusTerminals, name)
	}
	// Clocks require Event Mode (FMI 3.0 4.4).
	md.CoSimulation.HasEventMode = stringPtr("true")
	return nil
}

type terminalMemberVariable struct {
	VariableKind string `xml:"variableKind,attr"`
	VariableName string `xml:"variableName,attr"`
	MemberName   string `xml:"memberName,attr"`
}

type terminal struct {
	TerminalKind string                   `xml:"terminalKind,attr"`
	Name         string                   `xml:"name,attr"`
	MatchingRule string                   `xml:"matchingRule,attr"`
	Description  string                   `xml:"description,attr,omitempty"`
	Members      []terminalMemberVariable `xml:"TerminalMemberVariable"`
}

type terminalsAndIcons struct {
	XMLName    xml.Name   `xml:"fmiTerminalsAndIcons"`
	FmiVersion string     `xml:"fmiVersion,attr"`
	Terminals  []terminal `xml:"Terminals>Terminal"`
}

// WriteLsBusArtifacts writes terminalsAndIcons/terminalsAndIcons.xml and
// extra/org.fmi-standard.fmi-ls-bus/fmi-ls-manifest.xml into the FMU directory.
func WriteLsBusArtifacts(dir string, terminals []string) error {
	if len(terminals) == 0 {
		return nil
	}
	doc := terminalsAndIcons{FmiVersion: "3.0"}
	for _, name := range terminals {
		t := terminal{
			TerminalKind: "org.fmi-ls-bus.network-terminal",
			Name:         name,
			MatchingRule: "org.fmi-ls-bus.transceiver",
			Description:  "CAN Bus Terminal (Network Abstraction)",
		}
		for _, member := range []string{"Rx_Clock", "Rx_Data", "Tx_Clock", "Tx_Data"} {
			t.Members = append(t.Members, terminalMemberVariable{
				VariableKind: "signal",
				VariableName: name + "." + member,
				MemberName:   member,
			})
		}
		doc.Terminals = append(doc.Terminals, t)
	}
	data, err := xml.MarshalIndent(doc, "", "    ")
	if err != nil {
		return err
	}

	terminalsDir := filepath.Join(dir, "terminalsAndIcons")
	if err := os.MkdirAll(terminalsDir, os.ModePerm); err != nil {
		return err
	}
	data = append([]byte(xml.Header), append(data, '\n')...)
	if err := os.WriteFile(filepath.Join(terminalsDir, "terminalsAndIcons.xml"), data, 0644); err != nil {
		return err
	}

	extraDir := filepath.Join(dir, "extra", "org.fmi-standard.fmi-ls-bus")
	if err := os.MkdirAll(extraDir, os.ModePerm); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(extraDir, "fmi-ls-manifest.xml"), []byte(lsBusManifest), 0644)
}
