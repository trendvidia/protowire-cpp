// Copyright 2026 TrendVidia LLC
// SPDX-License-Identifier: MIT

package main

// godiff --rewrite <dir> runs every *.ops script in dir through pxf.Rewriter.
// cmd/pxf_diff/rewrite.cc is the same program for this port, and the two have
// to stay line for line alike.
//
// A script has one operation per line; blank lines and lines starting with #
// are skipped. Tokens are bare words or "quoted" strings with the escapes
// \n \r \t \\ \" and \xHH. A value or entry is the rest of the line, in PXF:
//
//	doc FILE                 start over on a document next to the script
//	src TEXT                 start over on the quoted source TEXT
//	set PATH VALUE           Set
//	remove PATH              Remove
//	replace NODE VALUE       ReplaceValue
//	span START END TEXT|!nil SetSpan
//	append NODE ENTRY        AppendEntry
//	keyedby PATH FIELD       KeyedByField
//	setkeyed PATH KEY SUB VALUE
//	rmkeyed PATH KEY SUB
//	rmelem PATH KEY
//	insert PATH KEY BEFORE ENTRIES
//	rename PATH OLD NEW
//	move PATH KEY BEFORE
//	bytes                    Bytes, and print the result
//	sweep FILE               every edit on every node of a document
//	sweep-glob PATTERN       sweep on each file matching PATTERN, sorted
//
// NODE walks Document() from the top: dot-separated entry indexes, with eN for
// element N of a list. The !forms build what PXF text cannot say; see value,
// entries and node.
//
// Each operation prints one line, op SEQ NAME: ok, or op SEQ NAME: err TEXT with
// TEXT quoted. bytes follows an ok with the document, one quoted line per line.

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"github.com/trendvidia/protowire-go/encoding/pxf"
)

func rwFatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(2)
}

type rwState struct {
	dir string
	seq int
	src []byte
	r   *pxf.Rewriter
}

func (x *rwState) result(name string, err error) bool {
	x.seq++
	if err != nil {
		fmt.Printf("op %d %s: err %s\n", x.seq, name, strconv.Quote(err.Error()))
		return false
	}
	fmt.Printf("op %d %s: ok\n", x.seq, name)
	return true
}

func (x *rwState) printBytes(name string, r *pxf.Rewriter) {
	out, err := r.Bytes()
	if !x.result(name, err) {
		return
	}
	for _, line := range strings.SplitAfter(string(out), "\n") {
		if line != "" {
			fmt.Printf("  | %s\n", strconv.Quote(line))
		}
	}
}

func (x *rwState) start(name string, src []byte) {
	x.src = src
	x.r = nil
	r, err := pxf.NewRewriter(src)
	if x.result(name, err) {
		x.r = r
	}
}

// rwToken takes the first token off s and returns it and the rest.
func rwToken(s string) (string, string) {
	s = strings.TrimLeft(s, " \t")
	if s == "" {
		rwFatal("missing argument")
	}
	if s[0] != '"' {
		if i := strings.IndexAny(s, " \t"); i >= 0 {
			return s[:i], strings.TrimLeft(s[i:], " \t")
		}
		return s, ""
	}
	var out []byte
	for i := 1; i < len(s); i++ {
		switch s[i] {
		case '"':
			return string(out), strings.TrimLeft(s[i+1:], " \t")
		case '\\':
			i++
			if i >= len(s) {
				rwFatal("bad quoted token: %s", s)
			}
			switch s[i] {
			case 'n':
				out = append(out, '\n')
			case 'r':
				out = append(out, '\r')
			case 't':
				out = append(out, '\t')
			case '\\', '"':
				out = append(out, s[i])
			case 'x':
				if i+2 >= len(s) {
					rwFatal("bad quoted token: %s", s)
				}
				n, err := strconv.ParseUint(s[i+1:i+3], 16, 8)
				if err != nil {
					rwFatal("bad quoted token: %s", s)
				}
				out = append(out, byte(n))
				i += 2
			default:
				rwFatal("bad quoted token: %s", s)
			}
		default:
			out = append(out, s[i])
		}
	}
	rwFatal("bad quoted token: %s", s)
	return "", ""
}

func rwParse(text string) *pxf.Document {
	doc, err := pxf.Parse([]byte(text))
	if err != nil {
		rwFatal("script text %q: %v", text, err)
	}
	return doc
}

func rwAssign(key string, v pxf.Value) *pxf.Assignment {
	return &pxf.Assignment{Key: key, Value: v}
}

func (x *rwState) value(rest string) pxf.Value {
	head, tail, _ := strings.Cut(rest, " ")
	switch head {
	case "!nil":
		return nil
	case "!bad":
		return &pxf.BadVal{}
	case "!list-bad":
		return &pxf.ListVal{Elements: []pxf.Value{&pxf.StringVal{Value: "a"}, &pxf.BadVal{}}}
	case "!list-nil":
		return &pxf.ListVal{Elements: []pxf.Value{nil}}
	case "!block-bad":
		return &pxf.BlockVal{Entries: []pxf.Entry{rwAssign("k", &pxf.BadVal{})}}
	case "!block-nil":
		return &pxf.BlockVal{Entries: []pxf.Entry{rwAssign("k", nil)}}
	case "!block-nilentry":
		return &pxf.BlockVal{Entries: []pxf.Entry{nil}}
	case "!block-nested-bad":
		// containsBadVal does not look inside a Block entry of a BlockVal.
		return &pxf.BlockVal{Entries: []pxf.Entry{
			&pxf.Block{Name: "n", Entries: []pxf.Entry{rwAssign("a", &pxf.BadVal{})}},
		}}
	case "!raw":
		text, _ := rwToken(tail)
		return &pxf.IntVal{Raw: text}
	case "!node":
		_, v := x.node(tail)
		return v
	}
	doc := rwParse("v = " + rest)
	return doc.Entries[0].(*pxf.Assignment).Value
}

func (x *rwState) entries(rest string) []pxf.Entry {
	head, tail, _ := strings.Cut(rest, " ")
	switch head {
	case "!none":
		return nil
	case "!nil":
		return []pxf.Entry{nil}
	case "!bad":
		return []pxf.Entry{rwAssign("a", &pxf.BadVal{})}
	case "!nil-value":
		return []pxf.Entry{rwAssign("a", nil)}
	case "!map-bad":
		return []pxf.Entry{&pxf.MapEntry{Key: "k", Value: &pxf.BadVal{}}}
	case "!block-bad":
		return []pxf.Entry{&pxf.Block{Name: "n", Entries: []pxf.Entry{rwAssign("a", &pxf.BadVal{})}}}
	case "!block-nilchild":
		return []pxf.Entry{&pxf.Block{Name: "n", Entries: []pxf.Entry{nil}}}
	case "!map", "!mapq":
		key, val := rwToken(tail)
		return []pxf.Entry{&pxf.MapEntry{Key: key, KeyQuoted: head == "!mapq", Value: x.value(val)}}
	case "!assignq":
		key, val := rwToken(tail)
		return []pxf.Entry{&pxf.Assignment{Key: key, KeyQuoted: true, Value: x.value(val)}}
	case "!blockq":
		name, body := rwToken(tail)
		return []pxf.Entry{&pxf.Block{Name: name, NameQuoted: true, Entries: x.entries(body)}}
	case "!type":
		url, _ := rwToken(tail)
		return []pxf.Entry{&pxf.TypeEntry{URL: url}}
	case "!q":
		text, _ := rwToken(tail)
		return rwParseEntries(text)
	}
	return rwParseEntries(rest)
}

// rwParseEntries parses inside a block, where a map entry is allowed.
func rwParseEntries(text string) []pxf.Entry {
	return rwParse("w {\n" + text + "\n}\n").Entries[0].(*pxf.Block).Entries
}

// node walks NODE, returning the Block it ends on or the value it ends on.
func (x *rwState) node(sel string) (*pxf.Block, pxf.Value) {
	switch sel {
	case "!nil":
		return nil, nil
	case "!handmade":
		return &pxf.Block{Name: "x"}, &pxf.IntVal{Raw: "9"}
	case "!bad":
		return nil, &pxf.BadVal{}
	}
	if x.r == nil {
		rwFatal("node %s with no document", sel)
	}
	entries := x.r.Document().Entries
	var block *pxf.Block
	var val pxf.Value
	for _, step := range strings.Split(sel, ".") {
		if step[0] == 'e' {
			n, err := strconv.Atoi(step[1:])
			if err != nil {
				rwFatal("bad node %s", sel)
			}
			list, ok := val.(*pxf.ListVal)
			if !ok || n >= len(list.Elements) {
				rwFatal("bad node %s", sel)
			}
			val = list.Elements[n]
		} else {
			n, err := strconv.Atoi(step)
			if err != nil || n >= len(entries) {
				rwFatal("bad node %s", sel)
			}
			block, val = nil, nil
			switch e := entries[n].(type) {
			case *pxf.Block:
				block, entries = e, e.Entries
			case *pxf.Assignment:
				val = e.Value
			case *pxf.MapEntry:
				val = e.Value
			default:
				rwFatal("bad node %s", sel)
			}
		}
		if bv, ok := val.(*pxf.BlockVal); ok {
			entries = bv.Entries
		}
	}
	return block, val
}

func (x *rwState) run(line string) {
	name, rest := rwToken(line)
	if name == "doc" || name == "src" || name == "sweep" || name == "sweep-glob" {
		arg, _ := rwToken(rest)
		switch name {
		case "doc":
			x.start(name, rwRead(filepath.Join(x.dir, arg)))
		case "src":
			x.start(name, []byte(arg))
		case "sweep":
			x.sweep(arg, rwRead(filepath.Join(x.dir, arg)))
		case "sweep-glob":
			files, err := filepath.Glob(filepath.Join(x.dir, arg))
			if err != nil || len(files) == 0 {
				rwFatal("no files match %s", arg)
			}
			sort.Strings(files)
			for _, f := range files {
				x.sweep(filepath.Base(f), rwRead(f))
			}
		}
		return
	}
	if x.r == nil {
		// The document was refused; nothing can run until the next one.
		return
	}
	r := x.r
	var err error
	switch name {
	case "set":
		path, v := rwToken(rest)
		err = r.Set(path, x.value(v))
	case "remove":
		path, _ := rwToken(rest)
		err = r.Remove(path)
	case "replace":
		sel, v := rwToken(rest)
		_, old := x.node(sel)
		err = r.ReplaceValue(old, x.value(v))
	case "span":
		a, tail := rwToken(rest)
		b, tail := rwToken(tail)
		start, e1 := strconv.Atoi(a)
		end, e2 := strconv.Atoi(b)
		if e1 != nil || e2 != nil {
			rwFatal("bad span: %s", line)
		}
		var text []byte
		if t, _ := rwToken(tail); tail != "!nil" {
			text = []byte(t)
		}
		err = r.SetSpan(start, end, text)
	case "append":
		sel, e := rwToken(rest)
		block, _ := x.node(sel)
		es := x.entries(e)
		if len(es) != 1 {
			rwFatal("append takes one entry: %s", line)
		}
		err = r.AppendEntry(block, es[0])
	case "keyedby":
		path, tail := rwToken(rest)
		field, _ := rwToken(tail)
		r.KeyedByField(path, field)
	case "setkeyed":
		path, tail := rwToken(rest)
		key, tail := rwToken(tail)
		sub, v := rwToken(tail)
		err = r.SetKeyed(path, key, sub, x.value(v))
	case "rmkeyed":
		path, tail := rwToken(rest)
		key, tail := rwToken(tail)
		sub, _ := rwToken(tail)
		err = r.RemoveKeyed(path, key, sub)
	case "rmelem":
		path, tail := rwToken(rest)
		key, _ := rwToken(tail)
		err = r.RemoveKeyedElement(path, key)
	case "insert":
		path, tail := rwToken(rest)
		key, tail := rwToken(tail)
		before, body := rwToken(tail)
		err = r.InsertKeyedElement(path, key, before, x.entries(body))
	case "rename":
		path, tail := rwToken(rest)
		from, tail := rwToken(tail)
		to, _ := rwToken(tail)
		err = r.RenameKeyedElement(path, from, to)
	case "move":
		path, tail := rwToken(rest)
		key, tail := rwToken(tail)
		before, _ := rwToken(tail)
		err = r.MoveKeyedElement(path, key, before)
	case "bytes":
		x.printBytes(name, r)
		return
	default:
		rwFatal("unknown operation: %s", line)
	}
	x.result(name, err)
}

// --- sweep -----------------------------------------------------------------

// fresh runs one edit on a new Rewriter over src and prints what it did.
func (x *rwState) fresh(label string, src []byte, edit func(r *pxf.Rewriter) error) {
	r, err := pxf.NewRewriter(src)
	if err != nil {
		rwFatal("sweep: %v", err)
	}
	if x.result(label, edit(r)) {
		x.printBytes(label+" bytes", r)
	}
}

func rwBody() []pxf.Entry {
	return []pxf.Entry{
		rwAssign("a", &pxf.IntVal{Raw: "1"}),
		&pxf.Block{Name: "n", Entries: []pxf.Entry{rwAssign("b", &pxf.StringVal{Value: "s"})}},
	}
}

func rwList() pxf.Value {
	return &pxf.ListVal{Elements: []pxf.Value{&pxf.IntVal{Raw: "1"}, &pxf.StringVal{Value: "s"}}}
}

func rwBlockVal() pxf.Value { return &pxf.BlockVal{Entries: rwBody()} }

func (x *rwState) sweep(name string, src []byte) {
	fmt.Printf("=== sweep %s\n", name)
	r, err := pxf.NewRewriter(src)
	if !x.result("new", err) {
		return
	}
	x.walk(src, "", r.Document().Entries, nil)
}

// walk sweeps entries, the scope at the dotted prefix. Indexes locate each
// node in a fresh Rewriter's own Document.
func (x *rwState) walk(src []byte, prefix string, entries []pxf.Entry, at []int) {
	for i, e := range entries {
		var key string
		var val pxf.Value
		var block *pxf.Block
		switch n := e.(type) {
		case *pxf.Block:
			key, block = n.Name, n
		case *pxf.Assignment:
			key, val = n.Key, n.Value
		case *pxf.MapEntry:
			key, val = n.Key, n.Value
		default:
			continue
		}
		if strings.Contains(key, ".") {
			continue
		}
		path := prefix + key
		here := append(append([]int(nil), at...), i)
		q := strconv.Quote(path)
		x.fresh("Remove "+q, src, func(r *pxf.Rewriter) error { return r.Remove(path) })
		x.fresh("Set int "+q, src, func(r *pxf.Rewriter) error { return r.Set(path, &pxf.IntVal{Raw: "7"}) })
		x.fresh("Set list "+q, src, func(r *pxf.Rewriter) error { return r.Set(path, rwList()) })
		x.fresh("Set block "+q, src, func(r *pxf.Rewriter) error { return r.Set(path, rwBlockVal()) })
		x.fresh("Set child "+q, src, func(r *pxf.Rewriter) error {
			return r.Set(path+".zz", &pxf.IntVal{Raw: "1"})
		})
		x.fresh("Set chain "+q, src, func(r *pxf.Rewriter) error {
			return r.Set(path+".y.z", rwList())
		})
		if block != nil {
			x.fresh("Append assign "+q, src, func(r *pxf.Rewriter) error {
				return r.AppendEntry(rwAt(r, here).(*pxf.Block), rwAssign("x", &pxf.IntVal{Raw: "1"}))
			})
			x.fresh("Append block "+q, src, func(r *pxf.Rewriter) error {
				return r.AppendEntry(rwAt(r, here).(*pxf.Block), &pxf.Block{Name: "n", Entries: rwBody()})
			})
			x.fresh("Append map "+q, src, func(r *pxf.Rewriter) error {
				return r.AppendEntry(rwAt(r, here).(*pxf.Block), &pxf.MapEntry{Key: "k k", Value: rwList()})
			})
			x.keyed(src, path, block.Entries, "")
			x.walk(src, path+".", block.Entries, here)
			continue
		}
		x.fresh("Replace "+q, src, func(r *pxf.Rewriter) error {
			var old pxf.Value
			switch n := rwAt(r, here).(type) {
			case *pxf.Assignment:
				old = n.Value
			case *pxf.MapEntry:
				old = n.Value
			}
			return r.ReplaceValue(old, rwList())
		})
		switch v := val.(type) {
		case *pxf.BlockVal:
			x.keyed(src, path, v.Entries, "")
			x.walk(src, path+".", v.Entries, here)
		case *pxf.ListVal:
			x.anon(src, path, v)
		}
	}
}

// rwAt returns the entry at indexes at in r's Document.
func rwAt(r *pxf.Rewriter, at []int) pxf.Entry {
	entries := r.Document().Entries
	var e pxf.Entry
	for _, i := range at {
		e = entries[i]
		switch n := e.(type) {
		case *pxf.Block:
			entries = n.Entries
		case *pxf.Assignment:
			if bv, ok := n.Value.(*pxf.BlockVal); ok {
				entries = bv.Entries
			}
		case *pxf.MapEntry:
			if bv, ok := n.Value.(*pxf.BlockVal); ok {
				entries = bv.Entries
			}
		}
	}
	return e
}

// anon sweeps a list of blocks as an anonymous keyed collection, keyed by the
// first element's first string field.
func (x *rwState) anon(src []byte, path string, list *pxf.ListVal) {
	if len(list.Elements) == 0 {
		return
	}
	first, ok := list.Elements[0].(*pxf.BlockVal)
	if !ok {
		return
	}
	field := ""
	for _, e := range first.Entries {
		if a, ok := e.(*pxf.Assignment); ok {
			if _, ok := a.Value.(*pxf.StringVal); ok {
				field = a.Key
				break
			}
		}
	}
	if field == "" {
		return
	}
	var keys []string
	var bodies [][]pxf.Entry
	for _, v := range list.Elements {
		bv, ok := v.(*pxf.BlockVal)
		if !ok {
			continue
		}
		for _, e := range bv.Entries {
			if a, ok := e.(*pxf.Assignment); ok && a.Key == field {
				if s, ok := a.Value.(*pxf.StringVal); ok {
					keys = append(keys, s.Value)
					bodies = append(bodies, bv.Entries)
					break
				}
			}
		}
	}
	x.keyedOps(src, path, keys, bodies, field)
}

// keyed sweeps a block's children as a keyed collection.
func (x *rwState) keyed(src []byte, path string, entries []pxf.Entry, field string) {
	var keys []string
	var bodies [][]pxf.Entry
	for _, e := range entries {
		switch n := e.(type) {
		case *pxf.Block:
			keys = append(keys, n.Name)
			bodies = append(bodies, n.Entries)
		case *pxf.Assignment:
			if bv, ok := n.Value.(*pxf.BlockVal); ok {
				keys = append(keys, n.Key)
				bodies = append(bodies, bv.Entries)
			}
		}
	}
	x.keyedOps(src, path, keys, bodies, field)
}

func (x *rwState) keyedOps(src []byte, path string, keys []string, bodies [][]pxf.Entry, field string) {
	do := func(label string, edit func(r *pxf.Rewriter) error) {
		x.fresh(label+" "+strconv.Quote(path), src, func(r *pxf.Rewriter) error {
			if field != "" {
				r.KeyedByField(path, field)
			}
			return edit(r)
		})
	}
	do("InsertKeyed end", func(r *pxf.Rewriter) error {
		return r.InsertKeyedElement(path, "ins-1", "", rwBody())
	})
	do("SetKeyed new", func(r *pxf.Rewriter) error {
		return r.SetKeyed(path, "fresh", "a.b", &pxf.IntVal{Raw: "1"})
	})
	for i, k := range keys {
		q := strconv.Quote(k)
		do("SetKeyed int "+q, func(r *pxf.Rewriter) error {
			return r.SetKeyed(path, k, "x", &pxf.IntVal{Raw: "1"})
		})
		do("SetKeyed chain "+q, func(r *pxf.Rewriter) error {
			return r.SetKeyed(path, k, "n.m", rwList())
		})
		for _, e := range bodies[i] {
			var sub string
			switch n := e.(type) {
			case *pxf.Block:
				sub = n.Name
			case *pxf.Assignment:
				sub = n.Key
			case *pxf.MapEntry:
				sub = n.Key
			}
			if sub == "" || strings.Contains(sub, ".") {
				continue
			}
			do("RemoveKeyed "+q+" "+strconv.Quote(sub), func(r *pxf.Rewriter) error {
				return r.RemoveKeyed(path, k, sub)
			})
			do("SetKeyed "+q+" "+strconv.Quote(sub), func(r *pxf.Rewriter) error {
				return r.SetKeyed(path, k, sub, &pxf.IntVal{Raw: "2"})
			})
		}
		do("RemoveElem "+q, func(r *pxf.Rewriter) error { return r.RemoveKeyedElement(path, k) })
		do("Rename quoted "+q, func(r *pxf.Rewriter) error { return r.RenameKeyedElement(path, k, "ren-1") })
		do("Rename bare "+q, func(r *pxf.Rewriter) error { return r.RenameKeyedElement(path, k, "ren1") })
		do("Move end "+q, func(r *pxf.Rewriter) error { return r.MoveKeyedElement(path, k, "") })
		do("Insert before "+q, func(r *pxf.Rewriter) error {
			return r.InsertKeyedElement(path, "ins-1", k, rwBody())
		})
		for _, o := range keys {
			do("Move "+q+" before "+strconv.Quote(o), func(r *pxf.Rewriter) error {
				return r.MoveKeyedElement(path, k, o)
			})
		}
	}
}

func rwRead(path string) []byte {
	b, err := os.ReadFile(path)
	if err != nil {
		rwFatal("%v", err)
	}
	return b
}

func rewriteMain(dir string) {
	files, err := filepath.Glob(filepath.Join(dir, "*.ops"))
	if err != nil || len(files) == 0 {
		rwFatal("no scripts in %s", dir)
	}
	sort.Strings(files)
	x := &rwState{dir: dir}
	for _, f := range files {
		x.src, x.r = nil, nil
		for i, line := range strings.Split(string(rwRead(f)), "\n") {
			if t := strings.TrimSpace(line); t == "" || t[0] == '#' {
				continue
			}
			fmt.Printf("=== %s:%d %s\n", filepath.Base(f), i+1, line)
			x.run(line)
		}
	}
}
