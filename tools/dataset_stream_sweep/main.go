// Copyright 2026 TrendVidia LLC
// SPDX-License-Identifier: MIT

// dataset_stream_sweep writes protowire-go's verdict on streaming @dataset
// inputs, for testdata/dataset-stream/go_sweep.tsv (#164): rows and headers
// cut short, junk where a row could begin, the cap on the bytes held while
// a row's end is sought, and the 4096-byte read boundary falling at every
// offset in and around a row. Each line is name, MaxMessageSize, mode (next or scan, which
// binds into google.protobuf.FileOptions), head, pad, tail and the result:
// the rows read, then EOF or the error. The input is head, then a row of pad
// 'p' characters in a string when pad >= 0, then tail; head and tail are hex.
//
//	cd tools/dataset_stream_sweep
//	GOWORK=off GOFLAGS=-mod=mod go mod edit -replace github.com/trendvidia/protowire-go=../../../protowire-go
//	GOWORK=off GOFLAGS=-mod=mod go run . ../../testdata/dataset > ../../testdata/dataset-stream/go_sweep.tsv
//
// The table above was written by protowire-go v1.16.1 (903b4a7).
package main

import (
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/trendvidia/protowire-go/encoding/pxf"
	"google.golang.org/protobuf/types/descriptorpb"
)

type tc struct {
	name string
	mms  int
	scan bool
	head string
	pad  int
	tail string
}

func (c tc) input() string {
	if c.pad < 0 {
		return c.head + c.tail
	}
	return c.head + "(\"" + strings.Repeat("p", c.pad) + "\")\n" + c.tail
}

func run(c tc) string {
	tr, err := pxf.UnmarshalOptions{MaxMessageSize: c.mms}.NewDatasetReader(strings.NewReader(c.input()))
	if err != nil {
		return "create: " + err.Error()
	}
	for i := 0; ; i++ {
		if c.scan {
			err = tr.Scan(&descriptorpb.FileOptions{})
		} else {
			_, err = tr.Next()
		}
		if err != nil {
			return fmt.Sprintf("rows=%d %v", i, err)
		}
	}
}

const h = "@dataset x.Row ( a )\n"

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: dataset_stream_sweep <testdata/dataset>")
		os.Exit(2)
	}
	var cases []tc
	for _, f := range []string{"truncated-row-eof", "truncated-row-then-rows", "junk-between-rows", "rows-end-at-directive"} {
		b, err := os.ReadFile(filepath.Join(os.Args[1], f+".pxf"))
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		cases = append(cases, tc{f, 0, false, string(b), -1, ""})
	}
	for _, c := range []struct{ name, rows string }{
		{"no close", "(\"yy\"\n"},
		{"newline in string", "(\"yy\n"},
		{"unterminated string at eof", "(\"yy"},
		{"unterminated bytes", "(b\"QUJD\n"},
		{"unterminated triple", "(\"\"\"abc\n"},
		{"open paren in row", "( 1 )\n( (2) )\n"},
		{"bare at", "( 1 )\n@ x\n"},
		{"at digits", "( 1 )\n@1\n"},
		{"directive", "( 1 )\n@foo x\n"},
		{"block comment open between rows", "( 1 )\n/* abc"},
		{"block comment open in row", "( 1 /* abc"},
		{"junk at eof", "( 1 )\nxyz"},
		{"brace at eof", "( 1 )\n}"},
		{"field entry", "( 1 )\nfoo = 1\n"},
		{"comments after rows", "( 1 ) # c\n// d\n/* e */\n( 2 )\n# end"},
		{"illegal in row", "( 1 $ )\n"},
		{"illegal between rows", "( 1 )\n$\n"},
		{"lone paren", "( "},
		{"close paren between rows", "( 1 )\n)\n"},
	} {
		cases = append(cases, tc{c.name, 0, false, h + c.rows, -1, ""})
	}
	// A header cut short or broken reports what Parse reports for it.
	for _, c := range []struct{ name, in string }{
		{"header cut", "@dataset x.Row ( a"},
		{"header cut after type", "@dataset x.Row"},
		{"header cols cut", "@dataset x.Row ( a, "},
		{"header string open", "@foo \"abc\n@dataset x.Row ( a )\n( 1 )\n"},
		{"header comment open", "/* abc\n@dataset x.Row ( a )\n"},
		{"header bytes open", "@foo b\"QUJD\n@dataset x.Row ( a )\n"},
		{"header illegal", "$ @dataset x.Row ( a )\n"},
		{"header paren in string", "@foo \"(\" @dataset x.Row ( a )\n( 1 )\n"},
		{"header proto body", "@proto x.Row { string a = 1; }\n@dataset x.Row ( a )\n( 1 )\n"},
		{"header proto paren", "@proto x.Row { // (\n string a = 1; }\n@dataset x.Row ( a )\n( 1 )\n"},
		{"header proto string open", "@proto x.Row { option x = \"a\n; }\n@dataset x.Row ( a )\n"},
		{"header proto cut", "@proto x.Row { string a = 1;\n"},
		{"header no dataset", "@foo x\n"},
		{"header empty", ""},
	} {
		cases = append(cases, tc{c.name, 0, false, c.in, -1, ""})
	}
	cases = append(cases,
		tc{"open row over a small cap", 56, false, h + "(\"", 10000, ""},
		tc{"open row over 5000", 5000, false, h + "(\"" + strings.Repeat("x", 10000), -1, ""},
		tc{"doc over a small cap", 30, false, h + "( 1 )\n( 2 )\n", -1, ""},
		tc{"rows past 4096, cap 4200", 4200, false, h + strings.Repeat("( 1 )\n", 2000), -1, ""},
		tc{"rows past 4096, cap 4096", 4096, false, h + strings.Repeat("( 1 )\n", 2000), -1, ""},
	)
	// Scan: a row read after the first 4096 bytes, whose body repeats the
	// column name and so is wider than MaxMessageSize, while the bytes held
	// are not.
	head := "@dataset google.protobuf.FileOptions ( php_metadata_namespace )\n"
	wide := "(\"" + strings.Repeat("v", 4480) + "\")\n"
	pad := 4096 - len(head) - len("(\"\")\n")
	cases = append(cases,
		tc{"scan body wider than the cap", 4500, true, head, pad, wide},
		tc{"scan body under the default cap", 0, true, head, pad, wide},
	)
	// The read boundary at 4096 falls at every offset in and around each row.
	targets := []struct{ name, rows string }{
		{"float", "( 1.5 )\n( 2 )\n"},
		{"neg inf", "( -inf )\n( 2 )\n"},
		{"string", "( \"abc\" )\n( 2 )\n"},
		{"triple", "( \"\"\"abc\"\"\" )\n( 2 )\n"},
		{"bytes", "( b\"QUJD\" )\n( 2 )\n"},
		{"timestamp", "( 2024-01-02T03:04:05Z )\n( 2 )\n"},
		{"duration", "( 5µs )\n( 2 )\n"},
		{"comments", "/* c */ ( 1 ) // d\n# e\n( 2 )\n"},
		{"directive", "( 1 )\n@foo x\n"},
		{"cut float", "( 1.5"},
		{"cut string", "( \"abc"},
		{"newline in string", "( \"ab\nc\" )\n"},
		{"junk", "( 1 )\nxyz\n"},
		{"open block comment", "( 1 )\n/* abc"},
	}
	for _, t := range targets {
		for k := 0; k <= 12; k++ {
			pad := 4096 - k - len(h) - len("(\"\")\n")
			for _, mms := range []int{0, 4200, 4100} {
				cases = append(cases, tc{fmt.Sprintf("boundary %s k=%d", t.name, k), mms, false, h, pad, t.rows})
			}
		}
	}
	for _, c := range cases {
		mode := "next"
		if c.scan {
			mode = "scan"
		}
		fmt.Printf("%s\t%d\t%s\t%s\t%d\t%s\t%s\n", c.name, c.mms, mode,
			hex.EncodeToString([]byte(c.head)), c.pad, hex.EncodeToString([]byte(c.tail)), run(c))
	}
}
