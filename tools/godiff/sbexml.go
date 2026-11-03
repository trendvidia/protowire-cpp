// Copyright 2026 TrendVidia LLC
// SPDX-License-Identifier: MIT

package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/trendvidia/protocompile"
	"github.com/trendvidia/protowire-go/encoding/sbe"
)

// sbeXML converts every .xml file in dir with XMLToProto and every .proto file
// with ProtoToXML, and prints each output or error. Line for line
// cmd/pxf_diff/sbexml.cc.
func sbeXML(dir, protoDir string) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	var names []string
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	for _, name := range names {
		switch {
		case strings.HasSuffix(name, ".xml"):
			data, err := os.ReadFile(filepath.Join(dir, name))
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(2)
			}
			fmt.Printf("=== xml %s\n", name)
			out, err := sbe.XMLToProto(data)
			if err != nil {
				fmt.Printf("error: %s\n", err)
				continue
			}
			fmt.Print(string(out))
		case strings.HasSuffix(name, ".proto"):
			fmt.Printf("=== proto %s\n", name)
			c := protocompile.Compiler{Resolver: protocompile.WithStandardImports(&protocompile.SourceResolver{
				ImportPaths: []string{dir, protoDir},
			})}
			files, err := c.Compile(context.Background(), name)
			if err != nil {
				fmt.Printf("compile error\n")
				continue
			}
			out, err := sbe.ProtoToXML(files[0])
			if err != nil {
				fmt.Printf("error: %s\n", err)
				continue
			}
			fmt.Print(string(out))
		}
	}
}
