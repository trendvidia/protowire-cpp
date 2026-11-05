// Copyright 2026 TrendVidia LLC
// SPDX-License-Identifier: MIT

// bigfloat_sweep writes protowire-go's verdict on pxf.BigFloat literals near
// powers of ten, near powers of two and at rounding carries, for
// testdata/bigfloat-literals/go_sweep.tsv (#144). Each row is max_exp,
// max_digits, literal, and what Unmarshal did: "err <text>", or "ok
// <mantissa> <exponent> <prec> <negative>" with a mantissa wider than 32
// bytes written as its length and FNV-1a 64 hash. The literals come from
// their construction, not from either port's arithmetic.
//
//	cd tools/bigfloat_sweep
//	GOWORK=off GOFLAGS=-mod=mod go mod edit -replace github.com/trendvidia/protowire-go=../../../protowire-go
//	GOWORK=off GOFLAGS=-mod=mod go run . ../../proto > ../../testdata/bigfloat-literals/go_sweep.tsv
//
// The rows at a 2^31-bit precision take protowire-go minutes each, so they
// run in parallel; the whole run takes a few minutes. On stderr it counts the
// literals whose big.Float.Parse exponent is not the exact value's.
package main

import (
	"context"
	"encoding/hex"
	"fmt"
	"hash/fnv"
	"math"
	"math/big"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"

	"github.com/trendvidia/protocompile"
	"github.com/trendvidia/protowire-go/encoding/pxf"
	"google.golang.org/protobuf/reflect/protoreflect"
)

// The schema of test/pxf_bignum_test.cc.
const wallet = `
syntax = "proto3";
package bignum.test;
import "pxf/bignum.proto";

message Wallet {
  pxf.BigInt    balance = 1;
  pxf.Decimal   price   = 2;
  pxf.BigFloat  rate    = 3;
}
`

type row struct {
	maxExp, maxDigits int
	lit               string
}

var (
	rows []row
	seen = map[row]bool{}
)

func add(maxExp, maxDigits int, lit string) {
	r := row{maxExp, maxDigits, lit}
	if !seen[r] {
		seen[r] = true
		rows = append(rows, r)
	}
}

var precs = []string{"", "p1", "p2", "p3", "p24", "p53", "p64", "p65", "p113", "p256", "p1000", "p13603"}

// decimalOf writes c 2^j to n significant digits, truncated, and nudged by
// delta units in the last place.
func decimalOf(c *big.Int, j, n int, delta int64) string {
	f, _ := new(big.Float).SetInt(c).Float64()
	e := int(math.Floor(math.Log10(f) + float64(j)*math.Log10(2)))
	for {
		q := e - n + 1
		num := new(big.Int).Set(c)
		den := big.NewInt(1)
		if j >= 0 {
			num.Lsh(num, uint(j))
		} else {
			den.Lsh(den, uint(-j))
		}
		t := new(big.Int).Exp(big.NewInt(10), big.NewInt(int64(max(q, -q))), nil)
		if q >= 0 {
			den.Mul(den, t)
		} else {
			num.Mul(num, t)
		}
		d := new(big.Int).Quo(num, den)
		switch l := len(d.String()); {
		case l > n:
			e++
		case l < n:
			e--
		default:
			return d.Add(d, big.NewInt(delta)).String() + "e" + strconv.Itoa(q)
		}
	}
}

func build() {
	// Powers of ten, either side of the bound and far past it.
	tens := []int64{0, 1, 27, 28, 29, 55, 56, 100, 300, 1000, 4930, 4931, 4932, 4933, 4934, 4935,
		4936, 4937, 4938, 4939, 4940, 4941, 10000, 20000, 100000, 332192}
	for _, k := range tens {
		for _, sign := range []int64{1, -1} {
			for _, m := range []string{"1", "5", "9.999", "1.5", "-2.5", "0.000123", "123456789012345678901234567890"} {
				for _, p := range precs {
					add(16384, 4096, fmt.Sprintf("%se%d%s", m, sign*k, p))
				}
			}
		}
	}
	// Past big.Float's range, at the int32 edges of the binary exponent, and
	// past int64.
	for _, s := range []string{
		"1e646456992", "1e646456993", "1e646456994", "1e-646456992", "1e-646456993", "1e-646456994",
		"1e-646457000", "1e-999999999", "1e999999999", "1e2147483646", "1e2147483647", "1e2147483648",
		"1e-2147483647", "1e-2147483648", "0.1e-2147483648", "0.01e-2147483648", "1e-2147483649",
		"1e9223372036854775807", "1e9223372036854775808", "1e-9223372036854775808", "1e-9223372036854775809",
		"1e-999999999999", "1e999999999999", "0e99999999999999999999", "0e-5", "0.0e9223372036854775807",
		"10e2147483646", "1000e-2147483650", "0.5e-646456993", "4.9e-646456993", "9e646456992",
	} {
		for _, p := range []string{"", "p2", "p64", "p13603"} {
			add(16384, 4096, s+p)
		}
	}
	pow2 := []int{0, 1, -1, 63, 64, -64, 1000, -1000, 16382, 16383, 16384, 16385, 16386, -16382, -16383,
		-16384, -16385, -16386, 66438, 66439, 66440, -332190, -332191, -332192, -332193}
	for _, m := range pow2 {
		// 2^m to 25 and 45 digits, nudged.
		for _, n := range []int{25, 45} {
			for _, d := range []int64{-1, 0, 1, 2} {
				lit := decimalOf(big.NewInt(1), m, n, d)
				for _, p := range precs {
					add(16384, 4096, lit+p)
				}
			}
		}
		// The carry at each precision: 2^m (1 - 2^-(p+1)), the tie that
		// rounds up to 2^m, to 30 to 90 digits and nudged.
		for _, p := range []int{1, 2, 3, 24, 53, 64, 113, 256} {
			b := new(big.Int).Lsh(big.NewInt(1), uint(p+1))
			b.Sub(b, big.NewInt(1))
			for _, n := range []int{30, 40, 60, 90} {
				for _, d := range []int64{-1, 0, 1} {
					add(16384, 4096, decimalOf(b, m-p-1, n, d)+"p"+strconv.Itoa(p))
				}
			}
		}
	}
	// Small bounds.
	for _, s := range []string{"0.125", "1", "2", "3", "7.9", "8", "1e-300", "1e300", "1e-100000", "1e100000", "1.5"} {
		for _, me := range []int{1, 2, 3, 100} {
			for _, p := range []string{"", "p1", "p2", "p64"} {
				add(me, 4096, s+p)
			}
		}
	}
	// Wide precisions, under a raised digit cap. protowire-go divides at
	// 2^31 bits for these, a minute or more each, and 1e-300p2147483647 did
	// not finish in half an hour, so only these few are here. The tests in
	// test/pxf_bignum_test.cc cover more literals at these precisions.
	for _, s := range []string{"1e-100000", "0.125", "-0.125"} {
		for _, p := range []string{"p2147483647", "p2147483648", "p4294967295"} {
			add(16384, 2000000000, s+p)
		}
	}
}

func verdict(md protoreflect.MessageDescriptor, r row) string {
	o := pxf.UnmarshalOptions{MaxNumericLiteralDigits: r.maxDigits, MaxBigFloatExponent: r.maxExp}
	msg, err := o.UnmarshalDescriptor([]byte("rate = "+r.lit+"\n"), md)
	if err != nil {
		return "err " + err.Error()
	}
	sub := msg.ProtoReflect().Get(md.Fields().ByName("rate")).Message()
	f := sub.Descriptor().Fields()
	m := sub.Get(f.ByName("mantissa")).Bytes()
	mant := hex.EncodeToString(m)
	if len(m) > 32 {
		h := fnv.New64a()
		h.Write(m)
		mant = fmt.Sprintf("%dB-fnv1a64-%016x", len(m), h.Sum64())
	}
	return fmt.Sprintf("ok %s %d %d %v", mant, sub.Get(f.ByName("exponent")).Int(),
		sub.Get(f.ByName("prec")).Uint(), sub.Get(f.ByName("negative")).Bool())
}

// parseDiffers reports whether big.Float.Parse's exponent is not that of
// the exact value rounded once, for a literal small enough to hold exactly.
func parseDiffers(lit string) bool {
	num, p, _ := strings.Cut(lit, "p")
	prec := uint64(256)
	if p != "" {
		var err error
		if prec, err = strconv.ParseUint(p, 10, 64); err != nil || prec > 1<<20 {
			return false
		}
	}
	bf, _, err := new(big.Float).SetPrec(uint(prec)).SetMode(big.ToNearestEven).Parse(num, 10)
	if err != nil || bf.IsInf() || bf.Sign() == 0 {
		return false
	}
	m, e, _ := strings.Cut(num, "e")
	k := int64(0)
	if e != "" {
		if k, err = strconv.ParseInt(e, 10, 64); err != nil || k > 400000 || k < -400000 {
			return false
		}
	}
	r, _ := new(big.Rat).SetString(m)
	t := new(big.Rat).SetInt(new(big.Int).Exp(big.NewInt(10), big.NewInt(max(k, -k)), nil))
	if k >= 0 {
		r.Mul(r, t)
	} else {
		r.Quo(r, t)
	}
	x := new(big.Float).SetPrec(uint(prec)).SetMode(big.ToNearestEven).SetRat(r)
	return x.MantExp(nil) != bf.MantExp(nil)
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: bigfloat_sweep <proto dir with pxf/bignum.proto>")
		os.Exit(2)
	}
	build()
	c := protocompile.Compiler{Resolver: protocompile.CompositeResolver{
		&protocompile.SourceResolver{Accessor: protocompile.SourceAccessorFromMap(map[string]string{"wallet.proto": wallet})},
		protocompile.WithStandardImports(&protocompile.SourceResolver{ImportPaths: []string{os.Args[1]}}),
	}}
	files, err := c.Compile(context.Background(), "wallet.proto")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	md := files[0].Messages().ByName("Wallet")

	out := make([]string, len(rows))
	differs := make([]bool, len(rows))
	next := make(chan int)
	var wg sync.WaitGroup
	for range min(runtime.NumCPU(), 8) {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range next {
				out[i] = verdict(md, rows[i])
				differs[i] = parseDiffers(rows[i].lit)
			}
		}()
	}
	for i := range rows {
		next <- i
	}
	close(next)
	wg.Wait()

	n := 0
	for i, r := range rows {
		fmt.Printf("%d\t%d\t%s\t%s\n", r.maxExp, r.maxDigits, r.lit, out[i])
		if differs[i] {
			n++
		}
	}
	fmt.Fprintf(os.Stderr, "%d literals; %d whose big.Float.Parse exponent is not the exact value's\n", len(rows), n)
}
