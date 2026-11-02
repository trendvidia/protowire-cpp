// Copyright 2026 TrendVidia LLC
// SPDX-License-Identifier: MIT

package main

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"fmt"
	"io"
	"os"

	"github.com/trendvidia/protowire-go/encoding/pb"
	"github.com/trendvidia/protowire-go/encoding/pxf"
	"github.com/trendvidia/protowire-go/encoding/sbe"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/dynamicpb"
)

// streamDecodes is enough to pass a stream's frames and see its error twice.
const streamDecodes = 6

type streamMsg struct {
	N    int64  `protowire:"1"`
	Text string `protowire:"2"`
	Data []byte `protowire:"3"`
}

// variants are the stream itself, then every prefix of it, then each byte set
// to each of four values: what a dropped or corrupted connection delivers.
func variants(stream []byte, emit func(label string, data []byte)) {
	emit("whole", stream)
	for n := range stream {
		emit(fmt.Sprintf("prefix %d", n), stream[:n])
	}
	for i := range stream {
		for _, b := range []byte{0x00, 0x7f, 0x80, 0xff} {
			c := bytes.Clone(stream)
			c[i] = b
			emit(fmt.Sprintf("byte %d=%02x", i, b), c)
		}
	}
}

func pbStreams(w io.Writer) {
	var buf bytes.Buffer
	enc := pb.NewEncoder(&buf)
	for _, m := range []*streamMsg{
		{N: 1, Text: "small"},
		{},
		{N: -2, Data: []byte{0xab, 0xcd}},
	} {
		if err := enc.Encode(m); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
	}
	fmt.Fprintf(w, "=== pb encoded %s\n", hex.EncodeToString(buf.Bytes()))
	for _, limit := range []int{0, 6} {
		variants(buf.Bytes(), func(label string, data []byte) {
			fmt.Fprintf(w, "--- pb limit %d %s\n", limit, label)
			dec := pb.NewDecoder(bytes.NewReader(data))
			if limit > 0 {
				dec.SetMaxFrameSize(limit)
			}
			for range streamDecodes {
				var m streamMsg
				if err := dec.Decode(&m); err != nil {
					fmt.Fprintln(w, status(err))
					continue
				}
				fmt.Fprintf(w, "ok %d %s %s\n", m.N, hex.EncodeToString([]byte(m.Text)), hex.EncodeToString(m.Data))
			}
		})
	}
}

// decoded prints a message that decoded.
func decoded(m *dynamicpb.Message) string {
	out, err := pxf.Marshal(m)
	if err != nil {
		return "ok " + status(err)
	}
	return "ok " + quote(string(out))
}

func sbeStreams(w io.Writer, root, dir, protoDir string) {
	s, err := compile(root, dir, []string{"sbe_stream.proto"}, protoDir)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	var file protoreflect.FileDescriptor
	for _, f := range s.files {
		if f.Path() == "sbe_stream.proto" {
			file = f
		}
	}
	codec, err := sbe.NewCodec(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	simple, order := s.find("stream.v1.Simple"), s.find("stream.v1.Order")

	m := dynamicpb.NewMessage(simple)
	if err := pxf.Unmarshal([]byte("id = 7\nvalue = -9\n"), m); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	o := dynamicpb.NewMessage(order)
	if err := pxf.Unmarshal([]byte("order_id = 5\nsymbol = \"ABC\"\nfills = [{fill_price = -1\nfill_qty = 2}]\n"), o); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	for _, c := range []struct {
		name string
		m    *dynamicpb.Message
	}{{"Simple", m}, {"Order", o}} {
		desc := c.m.Descriptor()
		var buf bytes.Buffer
		enc := codec.NewEncoder(&buf)
		for range 2 {
			if err := enc.Encode(c.m); err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(2)
			}
		}
		fmt.Fprintf(w, "=== sbe %s encoded %s\n", c.name, hex.EncodeToString(buf.Bytes()))
		variants(buf.Bytes(), func(label string, data []byte) {
			fmt.Fprintf(w, "--- sbe %s %s\n", c.name, label)
			dec := codec.NewDecoder(bytes.NewReader(data))
			for range streamDecodes {
				got := dynamicpb.NewMessage(desc)
				if err := dec.Decode(got); err != nil {
					fmt.Fprintln(w, status(err))
					continue
				}
				fmt.Fprintln(w, decoded(got))
			}
			if len(data) < 6 {
				return
			}
			fmt.Fprintf(w, "descriptor: ")
			got, err := codec.UnmarshalDescriptor(data[6:len(data)/2+3], desc)
			if err != nil {
				fmt.Fprintln(w, status(err))
				return
			}
			fmt.Fprintln(w, decoded(got))
		})
	}
}

func streams(root, dir, protoDir string) {
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	pbStreams(w)
	sbeStreams(w, root, dir, protoDir)
}
