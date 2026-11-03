// Copyright 2026 TrendVidia LLC
// SPDX-License-Identifier: MIT

package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"math/big"
	"os"
	"strconv"
	"strings"

	"github.com/trendvidia/protowire-go/encoding/pb"
)

type bigHolder struct {
	F big.Float   `protowire:"1"`
	D big.Rat     `protowire:"2"`
	I big.Int     `protowire:"3"`
	R []big.Float `protowire:"4"`
}

// bigLimits are the bounds a row is decoded under: the default, one a row can
// cross, and one past big.Float's range.
var bigLimits = []int{0, 10, 2147483647}

func floatValue(f *big.Float) string {
	return fmt.Sprintf("%d %t %d", f.Prec(), f.Signbit(), f.MantExp(nil))
}

// decodeBig prints what one decode gave. kind says what can be compared: a
// Decimal is compared by value, at the digits after the colon, and by its bytes.
// "past" is a value Go's Marshal refuses and this port's cannot yet (#93), or
// one it crashes on.
func decodeBig(w *bufio.Writer, kind string, data []byte, limit int, remarshal bool) {
	var h bigHolder
	if err := (pb.UnmarshalOptions{MaxBigFloatExponent: limit}).Unmarshal(data, &h); err != nil {
		fmt.Fprintln(w, status(err))
		return
	}
	line := "ok"
	base, digits, isDec := strings.Cut(kind, ":")
	if isDec {
		n, _ := strconv.Atoi(digits)
		line += " " + h.D.FloatString(n)
	} else {
		line += " " + floatValue(&h.F) + " " + h.I.String()
		for i := range h.R {
			line += " " + floatValue(&h.R[i])
		}
	}
	if remarshal && base != "past" {
		out, err := pb.Marshal(&h)
		if err != nil {
			line += " " + status(err)
		} else {
			line += " " + hex.EncodeToString(out)
		}
	}
	fmt.Fprintln(w, line)
}

// pbBig decodes every row of file under each bound and writes it back, then
// does the same to every damaged form of the rows that carry a BigFloat or a
// BigInt.
func pbBig(file string) {
	f, err := os.Open(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	defer f.Close()
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if strings.HasPrefix(sc.Text(), "#") {
			continue
		}
		p := strings.Fields(sc.Text())
		kind, label := p[0], p[1]
		data, err := hex.DecodeString(p[2])
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		for _, limit := range bigLimits {
			fmt.Fprintf(w, "=== %s %s limit %d\n", kind, label, limit)
			decodeBig(w, kind, data, limit, limit == 0)
		}
		if kind == "bf" || kind == "int" {
			variants(data, func(variant string, d []byte) {
				fmt.Fprintf(w, "--- %s %s\n", label, variant)
				decodeBig(w, kind, d, 0, true)
			})
		}
	}
}
