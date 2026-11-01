// Copyright 2026 TrendVidia LLC
// SPDX-License-Identifier: MIT

// godiff runs protowire-go over a manifest of PXF documents and prints what
// it did, one section per document. cmd/pxf_diff prints the same sections for
// this port, so scripts/go_differential.sh only has to diff two text files.
// The two programs have to stay line for line alike.
//
// Manifest: one document per line, as root TAB dir TAB protos TAB type TAB file. Paths are
// relative to root, protos is a comma-separated list, and
// type names the message of a document with no @type, or is "-".
package main

import (
	"bufio"
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"unicode"

	"github.com/trendvidia/protocompile"
	"github.com/trendvidia/protocompile/linker"
	"github.com/trendvidia/protowire-go/encoding/pxf"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/dynamicpb"
)

type schema struct{ files linker.Files }

func (s *schema) find(name string) protoreflect.MessageDescriptor {
	d, err := s.files.AsResolver().FindDescriptorByName(protoreflect.FullName(name))
	if err != nil {
		return nil
	}
	m, _ := d.(protoreflect.MessageDescriptor)
	return m
}

// FindMessageByURL resolves a URL to the message after its last '/'.
func (s *schema) FindMessageByURL(url string) (protoreflect.MessageDescriptor, error) {
	if m := s.find(url[strings.LastIndex(url, "/")+1:]); m != nil {
		return m, nil
	}
	return nil, errors.New("not found")
}

func compile(root, dir string, protos []string, protoDir string) (*schema, error) {
	c := protocompile.Compiler{Resolver: protocompile.WithStandardImports(&protocompile.SourceResolver{
		ImportPaths: []string{filepath.Join(root, dir), root, protoDir},
	})}
	files, err := c.Compile(context.Background(), protos...)
	if err != nil {
		return nil, err
	}
	return &schema{files}, nil
}

func quote(s string) string {
	return strings.NewReplacer(`\`, `\\`, "\n", `\n`).Replace(s)
}

func status(err error) string {
	if err == nil {
		return "ok"
	}
	// This tool's resolver supplies this text; a C++ resolver has no error.
	return quote(strings.TrimSuffix(err.Error(), ": not found"))
}

func typeOf(text string) string {
	for _, line := range strings.Split(text, "\n") {
		if t, ok := strings.CutPrefix(line, "@type "); ok {
			return strings.TrimSpace(t)
		}
	}
	return ""
}

func bytesOf(m proto.Message) string {
	b, err := proto.MarshalOptions{Deterministic: true}.Marshal(m)
	if err != nil {
		return "error"
	}
	return hex.EncodeToString(b)
}

func printText(label string, b []byte, err error) {
	if err != nil {
		fmt.Printf("%s: %s\n", label, status(err))
		return
	}
	fmt.Printf("%s:\n", label)
	for _, line := range strings.SplitAfter(string(b), "\n") {
		if line != "" {
			fmt.Printf("  | %s\n", quote(strings.TrimSuffix(line, "\n")))
		}
	}
}

func setFields(r *pxf.Result) []string {
	set := []string{}
	for _, p := range r.PresentFields() {
		if r.IsSet(p) {
			set = append(set, p)
		}
	}
	sort.Strings(set)
	return set
}

func runMessage(s *schema, d protoreflect.MessageDescriptor, text []byte) {
	opts := pxf.UnmarshalOptions{TypeResolver: s}
	plain := dynamicpb.NewMessage(d)
	fmt.Printf("unmarshal: %s\n", status(opts.Unmarshal(text, plain)))
	hooked := opts
	hooked.OnSecretField = func(path, value string) error {
		fmt.Printf("secret: %s = %s\n", quote(path), quote(value))
		return nil
	}
	m := dynamicpb.NewMessage(d)
	rr, herr := hooked.UnmarshalFull(text, m)
	fmt.Printf("hooked: %s\n", status(herr))
	if herr == nil {
		fmt.Printf("bytes: %s\n", bytesOf(m))
		fmt.Printf("set: %s\n", quote(strings.Join(setFields(rr), " ")))
	}
	raw := opts
	raw.SkipPostDecode = true
	rm := dynamicpb.NewMessage(d)
	_, serr := raw.UnmarshalFull(text, rm)
	fmt.Printf("skip-post: %s\n", status(serr))
	if serr == nil {
		fmt.Printf("bytes: %s\n", bytesOf(rm))
	}
	full := dynamicpb.NewMessage(d)
	r, err := opts.UnmarshalFull(text, full)
	fmt.Printf("full: %s\n", status(err))
	if err != nil {
		return
	}
	nulls := r.NullFields()
	sort.Strings(nulls)
	fmt.Printf("set: %s\n", quote(strings.Join(setFields(r), " ")))
	fmt.Printf("null: %s\n", quote(strings.Join(nulls, " ")))
	fmt.Printf("bytes: %s\n", bytesOf(full))
	b, err := pxf.MarshalOptions{TypeResolver: s}.Marshal(full)
	printText("marshal", b, err)
	b, err = pxf.MarshalOptions{TypeResolver: s, EmitDefaults: true}.Marshal(full)
	printText("marshal-defaults", b, err)
	b, err = pxf.MarshalOptions{TypeResolver: s, NullFields: r}.Marshal(full)
	printText("marshal-nulls", b, err)
}

func runDatasets(s *schema, doc *pxf.Document) {
	for _, ds := range doc.Datasets {
		d := s.find(ds.Type)
		fmt.Printf("dataset: %s (%s)\n", ds.Type, strings.Join(ds.Columns, ", "))
		if d == nil {
			continue
		}
		var out []byte
		out, err := pxf.AppendDatasetHeader(out, d, ds.Columns)
		printText("header", out, err)
		for i, row := range ds.Rows {
			m := dynamicpb.NewMessage(d)
			err := pxf.UnmarshalOptions{TypeResolver: s}.BindRow(m, ds.Columns, row)
			fmt.Printf("row %d: %s\n", i, status(err))
			if err != nil {
				continue
			}
			fmt.Printf("bytes: %s\n", bytesOf(m))
			b, err := pxf.AppendDatasetRow(nil, m, ds.Columns)
			printText("written", b, err)
		}
	}
}

func run(root, dir, protoDir, protos, deftype, file string) {
	fmt.Printf("=== %s\n", file)
	path := filepath.Join(root, file)
	text, err := os.ReadFile(path)
	if err != nil {
		fmt.Printf("read: %s\n", status(err))
		return
	}
	doc, perr := pxf.Parse(text)
	fmt.Printf("parse: %s\n", status(perr))
	if perr == nil {
		printText("format", pxf.FormatDocument(doc), nil)
	}
	if protos == "" {
		return
	}
	s, err := compile(root, dir, strings.Split(protos, ","), protoDir)
	if err != nil {
		fmt.Printf("compile: failed\n")
		return
	}
	if perr == nil && len(doc.Datasets) > 0 {
		runDatasets(s, doc)
		return
	}
	name := typeOf(string(text))
	if name == "" {
		name = deftype
	}
	d := s.find(name)
	if d == nil {
		fmt.Printf("type: none\n")
		return
	}
	fmt.Printf("type: %s\n", d.FullName())
	if perr == nil {
		pxf.CanonicalizeKeyed(doc, d)
		printText("canonical", pxf.FormatDocument(doc), nil)
	}
	runMessage(s, d, text)
}

// quoteRunes prints every rune through the quoter, so that the differential
// run covers all of Unicode rather than the runes a fixture happens to hold.
func quoteRunes() {
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	for r := rune(0); r <= unicode.MaxRune; r++ {
		if r >= 0xD800 && r <= 0xDFFF {
			continue
		}
		fmt.Fprintf(w, "%d %s\n", r, strconv.Quote(string(r)))
	}
	// Every prefix of a sequence, and the forms UTF-8 forbids.
	for _, bad := range []string{"\xff", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80",
		"\xe2\x82", "\xf0\x9f\x98", "a\x80b"} {
		fmt.Fprintln(w, strconv.Quote(bad))
	}
}

func main() {
	if len(os.Args) == 2 && os.Args[1] == "--quote-runes" {
		quoteRunes()
		return
	}
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: godiff <manifest> <proto-dir>")
		os.Exit(2)
	}
	f, err := os.Open(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		p := strings.Split(sc.Text(), "\t")
		run(p[0], p[1], os.Args[2], p[2], p[3], p[4])
	}
}
