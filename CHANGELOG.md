# Changelog

All notable changes to `protowire-cpp` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Each `protowire-*` repository has its own version line, so this version
number says nothing about any other port. See the Releases section of
`CONTRIBUTING.md`.

## [Unreleased]

## [2.1.0] — 2026-11-07

A minor release. `pxf::DatasetReader` now streams a `@dataset` table as protowire-go's reader does. A row cut short, junk between rows and a broken header are refused with the error `Parse` gives, at the same position, instead of ending the table early. Positions count from the start of the stream. The caller's limits and options apply to every row, and a row on a stream that stays open is returned as soon as it is complete. A comment that no entry follows is now kept, at the end of a document, before a block's `}` and between dataset rows. Minor rather than a patch, because it adds a public constructor and four public AST fields.

### Added

- **`pxf::Lexer(std::string_view input, Position start)`** (#161): a lexer over a piece of a larger document, whose positions and offsets count from `start`. `DatasetReader` uses it so a row's positions are the stream's.
- **`trailing_comments` on `Document`, `Block` and `BlockVal`, and `leading_comments` on `DatasetRow`** (#176), as protowire-go has them. The Fixed entry for #176 says what they hold.

### Fixed

- **`DatasetReader` reads a row under the caller's `max_bytes_literal_length`** (#160), as protowire-go does. A row's cells were parsed under the default `kMaxBytesLiteralLength` whatever the options said. A caller who lowered the limit still got a row whose `b"…"` cell was past it, and a caller who raised it had that cell refused. `0` still means the default. Only the nesting depth used to reach the row parser. The header (a `@header` block and the `@dataset` line) was parsed under the defaults for both limits. It is now parsed under the caller's `max_nesting_depth` and `max_bytes_literal_length`, so `Create` refuses such a header as protowire-go's `NewDatasetReader` does, with the same text and position.
- **`DatasetReader` positions count from the start of the stream** (#161), as protowire-go's do. A row's `pos`, its cells' positions and the line and column of an error from `Next` or `Scan` counted from the start of that row, so every row was at `1:1`. A refused row in a long stream now names its own line. `pxf::Lexer` gains a constructor that starts at a given `Position`, which the reader uses for each row.
- **A `@dataset` row cut short no longer reads as the end of the table** (#164), as protowire-go's streaming reader does. `DatasetReader::Next` and `Scan` ended the table cleanly at a row whose `)` never came, at an unterminated string, or at anything else where a row could begin. So an interrupted append read as a shorter, complete dataset, and the rows after a broken one were dropped. The rows now end only at the end of the input or at the next directive. Anything else is an error, and it is the error `Parse` reports for the same input, at the same position. This narrows what streams: the spec's `truncated-row-eof`, `truncated-row-then-rows` and `junk-between-rows` fixtures are now refused through the reader as well as by `Parse`. At the end of the input the reader consumes what is left, so `Tail()` is empty there.
- **`DatasetReader` caps the bytes it holds while looking for a row's end at `max_message_size`** (#164), as protowire-go does: `dataset input holds N bytes without a row boundary, MaxMessageSize=M`. A row that never closed was buffered whole, whatever the limit.
- **`DatasetReader::Scan` binds a row under the default `max_message_size`** (#164), as protowire-go does. The body a row is bound as repeats the column names, so it is wider than the row, and a row whose bytes fit the caller's limit could still be refused when bound. The row's bytes are still capped as they are read. The other limits apply as before.
- **A broken `@dataset` header is refused with the error `Parse` gives** (#164), as protowire-go does. `Create` said `no @dataset directive in stream` for a header cut short, and gave its own text for an unterminated string in it. The reader now finds the header's and each row's end with the lexer `Parse` uses rather than a byte scanner of its own. Streaming is about 15% faster on a 300,000-row table.
- **A timestamp cell right before a row's `)` parses** (#169), as protowire-go does. `(2024-01-02T03:04:05Z)` was refused with `invalid timestamp: 2024-01-02T03:04:05Z)`, because the lexer read the `)` into the timestamp. A space before the `)`, or a later cell, hid it.
- **`DatasetReader` reads a long token in time linear in its length** (#168), as protowire-go does. A string, bytes literal, comment or number spread over many reads was lexed again from its start on every 4 KiB read: an unclosed 1 MiB string cost 136 MB of lexing, and 2 MiB cost 540 MB. The scan now lexes a token still open again only once the bytes read since could end it, which is Go's `mayEndToken` rule, and the header scan resumes where it stopped too. The same 1 MiB string now costs 1.05 MB.
- **`DatasetReader` returns a complete row from a stream that stays open** (#171), as protowire-go does. On a pipe, a socket or a file still being appended to, `Next` could wait for 4 KiB more input, or for the stream to end, before returning a row that had already arrived. The read took a full 4 KiB (`std::istream::read`), and the scan held a token until four more bytes followed it. `Next` now reads what the stream has, blocking only when nothing is buffered. It takes a token as complete as soon as the bytes the lexer looks at past it have arrived. Input that ends reads as before.
- **A row's validator error and schema violation carry no row position** (#166), as protowire-go gives them. `DatasetReader::Scan` and `BindRow` put the row's position in front of every error, so a validator's refusal read `2:1: check: id: denied (...)` and a schema violation `2:1: PXF schema violations: ...`. Only a decode error is moved to the row now, and a hook's error from `on_secret_field` is one, so it keeps the row's position and its column. `Scan` also checks the column paths before the schema, as Go does, so a row type that breaks both gets the column's error.
- **`DatasetReader::Scan` honours every option but `max_message_size`, and protowire-go now matches** (#166; trendvidia/protowire-go#281). Go's `Scan` ignored `skip_validate`, `skip_post_decode`, `discard_unknown`, `on_secret_field` and `validator`, while cpp's honoured them. Go was changed to match (trendvidia/protowire-go#280), and cpp's behaviour stays. Each option now has a test that binds the same row through `Scan` and `BindRow`, with the texts Go gives, and `dataset_reader.h` lists the options `Scan` uses.
- **A comment that no entry follows is kept** (#176), as protowire-go keeps it (protowire-go#279, PR #283). The parser attached a comment only to the entry after it. So a comment at the end of a document was dropped, and one before a block's closing `}` was dropped or carried out of the block onto the next entry. Comments between dataset rows went to the next directive, or were dropped at the end. `Document`, `Block` and `BlockVal` now have `trailing_comments`, and `DatasetRow` has `leading_comments`, which `Parse` and `DatasetReader` both set. `FormatDocument` writes each in place, `CanonicalizeKeyed` keeps them when it rewrites a keyed block, and `Rewriter` copies them. A comment inside a row now comes before the row. Comments inside a list still move after it (protowire-go#282). Streaming a 300,000-row table is about 4% slower, and 9% slower when each row carries a comment, since those comments are now kept.

## [2.0.0] — 2026-11-05

A major release, because `pb::Marshal` now returns `StatusOr<std::vector<uint8_t>>` and every caller must change (#93). `pb::Marshal` and `pxf::Marshal` refuse what a decoder at the default limits refuses, instead of writing it. A PXF `BigFloat` literal is refused in protowire-go's words and order of checks, and a big-number message no longer counts as a nesting level. Several changes narrow what decodes, encodes or binds; each says so. This release also carries the change prepared as 1.2.1, which was never tagged.

### Added

- **`check_decode --format pxf-pb` and `pb-text`** (#110), the harness modes the spec's write gate (protowire#509) and text gate (protowire#503) read. `pxf-pb` writes a PXF document's PB bytes, and `pb-text` writes the PXF for a PB message. The `pxf` mode now honours `--limit MaxBigFloatExponent`.
- **`pb::MarshalOptions`, with `max_nesting_depth`** (#107, protowire#502), as protowire-go's `pb.MarshalOptions`. Pass it to `pb::Marshal` or to `pb::Encoder`'s constructor. `0` means `kMaxNestingDepth`. Raise it only when every reader raises `pb::UnmarshalOptions::max_nesting_depth` to match.
- **`pxf::MarshalOptions::max_nesting_depth`** (#107, protowire#502). `0` means `kMaxNestingDepth`. Raise it only when every reader raises `UnmarshalOptions::max_nesting_depth` to match.
- **SBE XML schema converters: `sbe::ParseXMLSchema`, `sbe::XMLToProto` and `sbe::ProtoToXML`** (#96), in `protowire/sbe/xml.h`, ported from protowire-go. Their output is protowire-go's byte for byte, checked by a new step of `scripts/go_differential.sh`. A `char` type in a non-UTF-8 `characterEncoding` imports as `bytes`, a `uint8` or `int8` array as `bytes` of its length, and a wider array is refused. XML is parsed with tinyxml2, which `protowire_sbe` already linked. The text of an error for malformed XML is tinyxml2's, not Go's.
- **`pb::Encoder` and `pb::Decoder`,** with `SetMaxFrameSize`, `kDefaultMaxFrameSize` and `kErrFramingCorrupt` (#83), protobuf's length-delimited framing over a `std::ostream` and `std::istream`. `Decoder` takes `UnmarshalOptions`, applied to each frame.
- **`sbe::Codec::NewEncoder` and `NewDecoder`,** with the same limits, framed by a Simple Open Framing Header, and **`sbe::Codec::UnmarshalDescriptor`,** which allocates the message (#83).
- **`pb::UnmarshalOptions::max_big_float_exponent`** (#93). `pb::Unmarshal` now refuses a `pxf.BigFloat` whose binary exponent is past `kMaxBigFloatExponent`, on both signs, before it stores the value (protowire#281). **This rejects messages that were accepted before.** `check_decode` passes `--limit MaxBigFloatExponent=N` to the pb decoder as well.
- **`pb::UnmarshalOptions::validator` and `report`** (#83), the seam `pxf` and `sbe` already have, on the decoded struct and on each frame of a `Decoder`. A validator overrides the new `check::Validator::ValidateStruct`, or derives from `check::StructValidator<T>`. An existing validator compiles unchanged, and returns an engine error for a struct. The seam is now in `protowire/check_seam.h`, which needs no protobuf; `protowire/check.h` includes it.
- **`IsEOF`, `IsUnexpectedEOF`, and `pb::IsFramingCorrupt` and `sbe::IsFramingCorrupt`,** which test a `Decode` result. `Decode` reports the end of a stream as a non-OK `Status`, as Go reports `io.EOF`.
- **`ParseTolerant`,** for editor tooling on a broken buffer. It returns a best-effort `Document` and every syntax error in source order, with a `BadVal` where a value is missing.
- **`EntrySpan` and `ValueSpan`.** Every AST node has an `end` position.
- **`TypeEntry` and `BadVal` in the AST.** An Any block's `@type = "..."` is now a `TypeEntry`, not an `Assignment` keyed `@type`. **A caller visiting `EntryPtr` or `ValuePtr` with no generic case must handle both.**
- **`KeyedDiagnostics`,** the keyed-field errors as positioned diagnostics over a parsed document.
- **The validator seam,** `protowire/check.h`: `check::Validator`, `check::Report`, `UnmarshalOptions::validator`, `Result::Report`, and `CodecOptions::validator` for SBE. A `Status` has only text, so `UnmarshalOptions::report` receives the report on failure.
- **`UnmarshalDescriptor` and `UnmarshalFullDescriptor`,** which allocate the message. They return a `shared_ptr` that keeps its factory alive.
- **`ApplyDefault`, `AppendValue` and `Result::PresentFields`.**
- **`pxf::Rewriter`, format-preserving edits to a PXF document** (#81). A port of protowire-go's `Rewriter`: `Set`, `Remove`, `ReplaceValue`, `SetSpan`, `AppendEntry`, `Bytes` and the keyed editors `KeyedByField`, `SetKeyed`, `RemoveKeyed`, `InsertKeyedElement`, `RemoveKeyedElement`, `RenameKeyedElement` and `MoveKeyedElement`. Everything outside an edited span round-trips byte for byte. `scripts/go_differential.sh` runs the same edits through both ports and compares every error and every output byte.
- **`UnmarshalOptions::on_secret_field`,** a hook that receives each `pxf.Secret` value in place of the message. As in protowire-go's `OnSecretField`.
- **`UnmarshalOptions::skip_post_decode`,** for a caller that runs its own required and default pass.
- **`AppendDatasetHeader` and `AppendDatasetRow` write `@dataset` tables** (#78). A cell holds what `Marshal` writes for the field, on one line, and an empty cell reads back as the unset field or the row is refused. On error `dst` is unchanged. Text and errors match protowire-go, and a column error now quotes the name as `strconv.Quote` does.
- **`MarshalOptions::null_fields`,** a `Result` whose nulls `Marshal` writes, for a message with no `_null` mask. As in protowire-go.
- **`scripts/go_differential.sh` runs protowire-go and this port over the spec's fixtures and `testdata/differential`,** and fails on any difference in verdict, error text, presence, bytes or output.
- **`pxf.Secret` sugar.** `pw = "x"` binds to `value`, in a singular field, a list element, a map value and a `@dataset` cell. `Marshal` writes that form when `hint` and `fingerprint` are empty, and the block form otherwise. **Output text changes for such a Secret.** `IsSecret` is new in `wellknown.h`.
- **`proto/pxf/secret.proto`,** the canonical `pxf.Secret` schema, which this port's copy of `proto/` lacked. The `testdata/dataset` fixtures import it.
- **`testdata/{any,bigfloat,blocks,duplicates,nulls,strings,dataset}`** and `test/pxf_corpus_test.cc`, which runs them through both decoders, `Parse`, `BindRow` and `DatasetReader`. `testdata/blocks` needed no code change: a `,` or `;` between the entries of a block was already rejected (protowire#312).
- **`@dataset` value cells** (protowire#341, v1.13). A cell holds any value, and `c = <value>` in a body means the same, so a list or block cell is accepted. `{}` is a present empty message.
- **`@dataset` dotted column paths** (protowire#342, v1.13), such as `(id, desc.energy)`. Cells sharing a prefix bind in one block, and a prefix with no non-empty cell leaves its sub-message absent. A header naming a path twice, a field beside a path inside it, or an empty segment is rejected when parsed, so `(a, a)` is too. A path through a repeated, map or scalar field, or into a type written as a literal, fails when a row binds.
- **`BindRow(msg, columns, row, opts)` and `DatasetReader::Create(src, opts)`,** for limits, `skip_validate` and the `type_resolver` a cell holding an Any block needs.
- **`FormatValue`,** which renders one AST value.
- **`kMaxBigFloatExponent` and `UnmarshalOptions::max_big_float_exponent`,** and `SetBigFloatFromLiteral`, which takes the limits. `check_decode` accepts `--limit MaxBigFloatExponent=N`.
- **`Token::prec`, `IntVal::prec` and `FloatVal::prec`** hold a number's precision suffix, and `FormatDocument` keeps it.

### Changed

- **A big-number message no longer counts toward `MaxNestingDepth` in `pb::Unmarshal`** (#148, decided by the owner), as in protowire-go. A `BigInt`, `Decimal` or `BigFloat` is a value, not a nested message, so a struct at depth 100 may hold one. This port refused that, and it read the bytes protowire-go writes for such a struct as an error. **This widens what decodes.** A struct nested past the limit is still refused.
- **Breaking: `pb::Marshal` returns `StatusOr<std::vector<uint8_t>>`** (#93, decided by the owner), as `pxf::Marshal` does and protowire-go's `Marshal` returns an error. **Every caller must change, so the next release is a major version.** Where code read `std::vector<uint8_t> b = pb::Marshal(v);`, it now checks `ok()` and takes `*b`. `Marshal` now refuses what a decoder at the default limits refuses, rather than write it, with protowire-go's text after the field it is in: a value nested past `kMaxNestingDepth` (`field next: ... nesting depth exceeds MaxNestingDepth=100`; #107, protowire#502), a `pxf.Decimal` whose scale, once reduced, is past `kMaxNumericLiteralDigits` (`pxf.Decimal scale 4097 exceeds MaxNumericLiteralDigits=4096`), and a `pxf.BigFloat` whose `prec` implies more digits than that (`pxf.BigFloat prec 13604 implies 4097 digits; ...`), whose exponent leaves `int32` once widened to `prec` (`pxf.BigFloat exponent -2147483698 does not fit int32`), or whose binary exponent is past `kMaxBigFloatExponent` (`pxf.BigFloat binary exponent 16385 exceeds MaxBigFloatExponent=16384`). Each of these used to be written in the form it was given, which every conformant decoder refuses. Depth is counted as `pb::Unmarshal` counts it: a submessage and a map entry are each one level. A `Decimal` with a scale below -4096 is a number of more than 4096 digits, which Go writes as an integer. This port refuses it, as `FormatDecimal` does, rather than build 10^2147483648 from scale -2^31. `pb::Encoder::Encode` returns the refusal and writes no frame.
- **A decoder rejects `null` on a field with `(pxf.required)` or `@required`** (#131, decided by the owner; trendvidia/protowire#569, protowire-go#268). **This narrows what decodes.** A required field holds a value, so `null` is now rejected as absence is, whatever the field's type and presence. Before, `null` counted as present, so `r = null` on `optional int32 r [(pxf.required) = true]` decoded and left a required field with no value. The error is `null is not allowed on required field "r": a required field holds a value`, in a body, a nested body, a `@dataset` cell and a dotted cell, from `Unmarshal` as well as `UnmarshalFull`. A plain scalar with `(pxf.required)`, which already rejected `null`, now names the field as required instead of suggesting `optional`. A zero value still satisfies the field. A `_null` mask that names an unset required field no longer makes `AppendDatasetRow` write `null` for it: the writer refuses it, as it does without the mask.
- **`pxf::Marshal` writes an unset field that has `(pxf.default)` or `(pxf.required)` so that it reads back** (#129, protowire-go#259; draft -01 § Common Schema Layer, "`decode_PXF(encode_PXF(v)) == v`"), as protowire-go does. This changes what Marshal writes for annotated fields, and narrows what encodes. Marshal left out every unset field, so a reader that applies defaults filled it in: `int32 n = 1 [(pxf.default) = "5"]` holding 0 read back as 5, and a required scalar holding 0 made the full decode refuse the document. A proto3 scalar without presence is now written as its zero value, a required list or map as `[]` or `{}`, and a message-typed field with a default, such as a wrapper or a Timestamp, as `null`. An unset `optional` or oneof scalar with a default is still left out and reads back as its default, the owner's choice in protowire-go#267. An unset `optional` field that is `(pxf.required)` is now an error, `pxf: <full name> is unset, and the field is (pxf.required)`; with `emit_defaults` it is written as zero, as before. An Any whose body has such a field stays as `type_url` and `value`. Inside a `@dataset` value cell, the same error now names the field by its full name. Unannotated fields are unchanged.
- **`sbe::Codec::New` refuses a field in a oneof, in a message or a composite** (#141), as protowire-go does, with `sbe: oneof field <M>.<f> not supported`, or `composite <C> contains oneof field <f>` under the caller's prefix. **This narrows what binds:** such a message used to bind with a block slot for every member, and `Marshal` wrote the unset ones as zero. A proto3 `optional` field still binds, and so does a oneof in a group entry, which Go does not check either.
- **A PXF `BigFloat` literal is refused in protowire-go's words and in its order of checks** (#144, decided by the owner). The port now follows `big.Float.Parse` step by step, including the power of five it rounds at `prec + 64` bits, and asks Go's questions of that result in Go's order: `is above big.Float's range` / `is below big.Float's range`, then the `prec` cap, then `is below the wire's exponent range`, then `has binary exponent <e>, and MaxBigFloatExponent=<N> bounds its magnitude` with Parse's exponent. Then it asks the bound and the wire range again of the exact value. A literal far past the bound used to get this port's own `is past MaxBigFloatExponent=16384`. `1e-100000p64` now reports `has binary exponent -332192`, `1e-646456994` reports `is below big.Float's range`, and `1e-999999999999`, whose binary exponent leaves the int32 range before Parse applies the power of five, reports `invalid big float`. **This narrows what decodes at the bound:** a literal a hair from a rounding carry, whose Parse exponent is one past the exact value's, is now refused as Go refuses it. Before, it was taken. A clipped literal in these messages is now Go's first 24 bytes and `…`, not 40 bytes and `...`. `testdata/bigfloat-literals/go_sweep.tsv` holds protowire-go's verdict on 9,393 literals, and a test compares every row. A precision suffix quoted in the decoder's `is allowed only on a pxf.BigFloat` and `a key cannot carry a precision suffix` errors is clipped the same way.
- **`sbe` refuses a value its `(sbe.encoding)` primitive cannot hold, in both directions** (#133, protowire-go#252; draft -01 § SBE Annotations, "Emitters MUST reject values that fall outside the narrowed type's range"). This narrows what encodes, decodes and binds. `Marshal` used to keep the low bits: `int32 v [(sbe.encoding) = "int8"]` set to 300 was written as 44, and read back as 44 with no error. It now refuses the value with `sbe: field m.v1.M.v: value 300 is outside the range of int8, [-128, 127]`, for every integer primitive, and a `double` narrowed to `float` past ±3.4028234663852886e+38 (NaN and the infinities still pass). An enum is laid out as `uint8`, so an enum value past 255, or a negative one, is refused too. `Unmarshal` refuses a wire value the field cannot hold, such as `uint32` 2^31 into an `int32` field, with `sbe: field m.v1.M.v: wire value 2147483648 (uint32) does not fit a int32 field, [-2147483648, 2147483647]`; it used to cut it to the field's low bits. A signed primitive is now sign-extended whatever the field, so `int8` 0xFF in a `uint32` field is -1 and refused, where it used to read as 255. `Codec::New` refuses `(sbe.encoding)` on a `string` or `bytes` field, and a primitive that crosses between integers and floating point (`int32` with `"float"`, `double` with `"int32"`). A `double` field under `"float"`, and a `float` field under `"double"`, used to write and read the wrong bits; they now convert the value.
- **`sbe::Codec::New` refuses a schema the SBE header cannot describe, and `Marshal` a group it cannot count** (#132, protowire-go#250). This narrows what binds and what encodes. Every SBE header field is a 16-bit number, and the codec kept the low 16 bits of a larger one. `bytes b = 1 [(sbe.length) = 65537]` bound as a 1-byte field. A block longer than 65535 bytes, in a message, a group entry or a composite, bound with a wrapped length and offsets. A `(sbe.template_id)`, `(sbe.schema_id)` or `(sbe.version)` of 65537 was written as 1. `Codec::New` now refuses each of these with protowire-go's text, naming the field, message or file, such as `sbe: field m.v1.M.b: (sbe.length) = 65537 is past 65535, the largest blockLength SBE can state`; 65535 still binds. `Marshal` refuses a repeated field with more than 65535 entries, with `sbe: field m.v1.M.e has 65536 entries, past 65535, the largest numInGroup SBE can state`: 65536 entries used to be written with a count of 0, and the message read back empty with no error. Every other bind error of a template now has protowire-go's text as well, prefixed with the field, group or composite it is about, such as `sbe: field m.v1.M.s: string field requires (sbe.length) annotation` where it used to be `string/bytes field requires (sbe.length)`.
- **`sbe` tells two schemas' template ids apart** (#134, protowire-go#253 and #257; draft -01 § SBE Annotations, "sbe.template_id MUST be unique among messages within a schema", and § Header and Block Length). This narrows what binds and what decodes. `Codec::New` refuses two different messages with one `(sbe.template_id)` in one schema, with `sbe: messages A and B share (sbe.template_id) = 7 in schema 1; it must be unique within a schema`; the second used to replace the first, and `NewView` then read one message's bytes as the other's. A file passed twice still binds, and so do two schemas that each use a template id. `NewView` now finds a template by the header's schema id and template id together, and refuses an unknown pair with `sbe: unknown template ID 9 in schema 2`, even if another schema has that template id; it used to say `unknown template id 9`. `Unmarshal` and `UnmarshalDescriptor` refuse a header whose schema id is not the target message's, with `sbe: schema ID mismatch: got 1, want 2 for template 7`; the version is not checked. A caller that builds headers by hand must now set the schema id.
- **`pxf::Marshal` refuses a message nested past `kMaxNestingDepth`** (#107, protowire#502), as protowire-go does (protowire-go#236), with `pxf: nesting depth exceeds MaxNestingDepth=100`. Depth is counted as a decoder counts it: the root is depth 0, and each `{` or `[` is one level, so a list of lists stops at 50. It used to write a document that a reader at the same limit refused. **This narrows what encodes.** `AppendDatasetRow` refuses too. An Any whose body would be too deep stays as `type_url` and `value`, so an Any's contents never cause a refusal; only an Any that itself sits past the limit does. An Any's value is also no longer cut off at libprotobuf's 100-level parse limit when the writer's limit is raised. `pb::Marshal` refuses too; see its entry above.
- **A schema that redefines a well-known type with the wrong shape is refused at bind time** (draft -01 §wkt-shapes, protowire#487, protowire-go#218). This narrows what binds. A message named, say, `pxf.Secret` or `google.protobuf.Timestamp` without the fields the codec reads now gives a violation of the new kind `ViolationKind::kWellKnownShape`. Copies of the canonical schemas are unaffected, and field numbers are not checked. The check also reaches a schema's own file at a `google/protobuf/` path, which the closure walk used to skip.
- **`ReadBigFloatAsString` returns `StatusOr<std::string>`,** so it can refuse a value.
- **`Violation::name` holds the `(pxf.default)` literal for `(pxf.required)` with `(pxf.default)` on one field** (protowire-go#211). It was empty.

### Removed

- **`src/pxf/decode.cc`,** an AST decoder with no caller. `DecodeDocumentInto` was declared in no header, and it had none of the spec rules added to `decode_fast.cc`.

### Fixed

- **A PXF `BigFloat` literal with a precision suffix of hundreds of millions of bits no longer takes minutes** (#151, decided by the owner: option 3, protowire-go#274). This is possible only with `max_numeric_literal_digits` raised past the suffix's digits; `p2147483647` implies about 646 million. The port follows `big.Float.Parse` only to place a literal against the range checks, and `Parse`'s cost grows with the square of its precision. It is now followed at no more than 16384 bits, as protowire-go does, so `1e-300p2147483647` is refused in milliseconds with the same error. At the default limits a suffix is at most `p13603`, so nothing is read differently. All 9,393 rows of `go_sweep.tsv`, nine of them above 16384 bits, are unchanged, and protowire-go with #274 writes the same table.
- **`pxf::FormatDocument` keeps every directive** (#130, protowire#558), as protowire-go does (protowire-go#262). It wrote only `@type`, comments and entries. So a `@dataset` document formatted to nothing and lost every row, and `@entry`, `@proto` and named directives were dropped from any document. It now writes them in document order, each after its comments. `@type` comes first, since it binds no anonymous `@proto`. A dataset is written as its header, then one row per line, with an absent cell left empty and `null` written. A named directive's body and a `@proto` body are written as they were read. A document whose only directive is `@type` formats as before.
- **`pxf::Marshal` refuses a `BigFloat` whose value rounded to its `prec` is past `kMaxBigFloatExponent`** (#128, protowire#560), as protowire-go does (protowire-go#261), with `pxf.BigFloat binary exponent 16385 after rounding to prec 3 exceeds MaxBigFloatExponent=16384`. Rounding can carry: a 54-bit all-ones mantissa at exponent 16330 is at the bound as stored and past it at prec 3. It used to be written as `1.2e+4932p3`, which no reader accepts. **This narrows what encodes.** A `@default` carrier's `big_float_value` is refused the same way. Its three limit refusals now use protowire-go's words. Both paths also refuse a zero whose `prec` renders past `MaxNumericLiteralDigits`, as Go does; it used to be written as `0p<prec>`.
- **`check_decode` prints a PXF rejection with its position** (#145), `reject: 3:3: unknown field ...`, as protowire-go's does, for `--format pxf` and `pxf-pb`.
- **`sbe::Codec::Marshal` cuts an overlong string on a character boundary** (#105, protowire#495), as protowire-go does (protowire-go#246). A `string` longer than its `(sbe.length)` is cut at the last whole UTF-8 character that fits and NUL-padded. A byte-exact cut could leave half a character, and then every strict decoder rejected the whole message. A `bytes` field is still cut at the limit. **The bytes written for such a string change.**
- **A PXF `nan` is stored as the canonical quiet NaN, `0x7FF8000000000000`** (#113, protowire#512), as protowire-go now does (protowire-go#241). It was Go's old `math.NaN()`, `0x7FF8000000000001`. That covers values, list elements, `DoubleValue`, and `(pxf.default)`. **The PB and SBE bytes of a double `nan` read from text change.** Float bytes don't change. `pb::Marshal` also writes any NaN in a `double` or `float` struct field canonically: singular, in a list, and as a map value. A message's fields are written as they are, so a NaN read from PB keeps its payload.
- **`pb::Unmarshal` refuses a `pxf.BigFloat` whose `prec` is past the precision-suffix bound** (#108, protowire#499). That is a `prec` whose literal needs more than `max_numeric_literal_digits` digits: above 13603 bits at the default, zeros included. It is checked before the value is built. An 11-byte message at prec 2^31-1 used to decode, and writing it back widened the mantissa to 256 MiB. **This rejects messages that were accepted before.** `UnmarshalOptions::max_numeric_literal_digits` moves the bound. `pb::Marshal` refuses a hand-built value past it.
- **A `BigFloat` `prec` varint wider than 32 bits keeps its low 32 bits** (#106), as a protobuf reader does and protowire-go now does (protowire-go#230). It was clamped to 2^32-1, so 2^32 read as 2^32-1 bits where every other reader reads prec 0, a zero. **The same bytes now decode to a different value.**
- **A PXF `BigFloat` literal wider than 2^31-1 bits is refused** (protowire-go#231), with Go's `precision p<N> exceeds the widest mantissa big.Float can hold, 2147483647 bits`. Only a `max_numeric_literal_digits` raised past 646 million admits such a suffix. It used to build the mantissa: 256 MiB at 2^31 bits. A zero is still accepted.
- **`sbe::Codec::New` diagnoses a stale descriptor, in protowire-go's words** (#122). A file or message that carries `(sbe.schema_id)` or `(sbe.template_id)` at the pre-v1.12.0 option numbers (50100, 50200) now gets an error saying it must be recompiled. A stale message used to be skipped silently, as if it were not a template. **Error text changes:** a file with no schema id now says `sbe: file <name> missing (sbe.schema_id) option`, as Go does.
- **`sbe::Codec::Unmarshal` keeps the sign of a narrow signed encoding** (#120). An `int32` or `int64` field with `(sbe.encoding)` of `int8`, `int16` or `int32` decoded a negative value as a large positive one: -1234 in an `int16` came back as 64302. It is now sign-extended, as protowire-go and `sbe::View` read it. **Such values now decode differently.**
- **A PXF Decimal `-0` or `-0.00` keeps its sign** (#110), as protowire-go reads and writes it. It was read with `negative` unset and written back as `0`. **The PB bytes for such a literal change** (`negative` is now set). The spec's write gate found the difference.
- **`pxf::Marshal` writes a Duration past the literal's range as a block** (#116), as protowire-go does (protowire-go#226). A literal holds 64 bits of nanoseconds, about 292 years. A longer Duration was written as `<duration overflow>`, which no reader accepts. It is now `f { seconds = 9500000000 }`, which keeps the exact value, in a field, list, map or `@dataset` cell. Shorter Durations are written as before.
- **`pxf::Marshal` no longer writes an Any body that changes the message or cannot be read** (#102). With a `type_resolver`, an Any gets `@type` and a body only if reading that body gives back the same bytes, as in protowire-go. Otherwise it stays as `type_url` and `value`. An Any at `kMaxNestingDepth` is not unpacked.
- **`pb::Unmarshal` rejects invalid UTF-8 in a `std::string` field** (#94), with `invalid UTF-8 in string`, as HARDENING.md § UTF-8 requires and protowire-go now does. That covers singular fields, list elements, nested messages, map keys and map values, and each frame of a `Decoder`, which goes on to the next frame. **This rejects messages that were accepted before.** A field that holds arbitrary bytes should be `std::vector<uint8_t>`. `sbe::View` is unchanged, as in Go.
- **`pb::Marshal` and `pb::Unmarshal` read and write `pxf.BigFloat` and `pxf.Decimal` as protowire-go and the schema do** (#93). `BigFloat.exponent` and `Decimal.scale` are `int32`, a plain varint, and this port used zigzag. **The bytes this port writes change, and it reads bytes from earlier versions of itself as a different number.** A negative exponent or scale went out in a form every other decoder misread: -5 as 9, which Go reads as +9. Messages stored by an earlier version must be rewritten; there is no flag that reads the old form. `BigFloat` also goes out in Go's one form per value: the mantissa is rounded to `prec`, half to even, and widened to a `prec`-bit integer, a zero has no exponent, and -0 at prec 0 keeps its sign, where it was dropped. **So a `BigFloat` read back can hold different fields for the same number.** `Unmarshal` rounds a mantissa wider than `prec` and drops the fields of a zero.
- **`pb::Marshal` writes a `pxf.Decimal` reduced, as protowire-go does** (#97). `{100, 2}` goes out as `{1, 0}` and `{5, -2}` as `{500, 0}`, and a zero at any scale as a zero. The bytes change and the number does not. A `pb::Decimal` keeps its own fields, and PXF text keeps a literal's scale, so `1.00` stays `1.00` there. A scale past ±4096 once reduced is refused (#93).
- **A `BigInt` or `Decimal` decodes to a value** (#93): leading zero bytes go, and -0 is 0, as in protowire-go. Writing it back gives Go's bytes.
- **`pb::Unmarshal` reads the wire as protowire-go does** (#83). Field number 0 and a varint past 64 bits are rejected, **which rejects input that was accepted before.** An unknown group is skipped, which was an error. **Error text changes:** a failure names its field, `field text: corrupt string`, and uses Go's words, such as `corrupt int` and `corrupt field 15`.
- **An SBE string keeps a zero inside it** (#83). Decoding, and `View::String`, stopped at the first zero; now only trailing zero padding goes. **A string with a zero inside it now decodes differently.** `bytes` fields keep the whole region, as before. SBE error text gains Go's detail, such as `sbe: data too short for root block: need 16, have 12`.
- **Every file in the import closure is validated, at a `google/protobuf/` path too** (#90). A schema's own file at such a path escaped the reserved-name and annotation checks, and at `google/protobuf/descriptor.proto` it escaped all of them. Only the `descriptor.proto` linked into libprotobuf is skipped, since it cannot be a schema's own. **A schema with a violation in such a file is now rejected.** Fields with no options skip the annotation lookups, so a schema outside the annotations decodes faster than before. A dynamically built schema that imports them pays a fixed ~0.5 µs per decode to walk its compiled `descriptor.proto`; `skip_validate` still avoids all of it.
- **A redefined well-known type no longer crashes** (#89). `Unmarshal`, `UnmarshalFull` and `Marshal` segfaulted or aborted on such a message: 19 of the 45 documents in `testdata/differential/wkt-shape`. The codec now checks the shape as well as the name, so where validation does not run, as in `Marshal` and with `skip_validate`, the message is ordinary and has no literal form. `on_secret_field` does not fire for a `pxf.Secret` of the wrong shape, and a `pxf.Secret` block naming a `hint` or `fingerprint` the schema does not declare is an error.
- **A timestamp follows Go's `time.Parse`, not the RFC.** A one-digit hour (`T3:04:05Z`), a `,` before the fraction, a fraction past nine digits (cut, not rounded) and an offset up to `24:60` now read. A lower-case `t` or `z` and an offset past that are rejected. **This rejects documents that were accepted before.**
- **`MaxNumericLiteralDigits` leaves out leading zeros** (protowire#410), for `pxf.BigInt`, `Decimal` and `BigFloat`, so a literal with a long run of them no longer fails. A `BigFloat` literal may open with `+` or `.`, as `big.Float` allows, and `inf` is reported as out of range.
- **A `(pxf.default)` is read as protowire-go reads it, by `strconv` and package `time`.** `+5`, `nan`, `Infinity` and hex floats now work. The errors carry the text Go wraps, such as `strconv.ParseInt: parsing "x": invalid syntax`. A rejected wrapper or big number default no longer leaves an empty message on the parent, so `HasField` stays false.
- **`Parse` matches protowire-go on more errors:** the position of the nesting limit, `@type` at the top level, `@type` beside `@dataset`, and a key with a precision suffix. **Text a caller matched on changes.** A user-supplied name or value in a decoder error is escaped as Go's `%q` does.
- **A trailing comment stays with its entry.** `a = 1 # note` became a leading comment of the next entry, or was dropped at the end of a block. **`FormatDocument` output changes for such a document.**
- **`body_offset` after a source or descriptor `@proto` is the end of the token.** It could be short by the literal's quotes and escapes. The brace matcher is protowire-go's.
- **The token kind prints as `@directive`,** not `@<directive>`, in error text.
- **`null` binds only to a field with explicit presence** (protowire#339, #379, #380). On a proto3 scalar without `optional`, a proto2 `required` field, or a repeated or map field it is now rejected, in a `@dataset` cell too. Such a field has no unset state, so `null` read back as its zero value. **This rejects documents that were accepted before:** declare the field `optional`, or as a wrapper type.
- **A raw line feed in a simple string is rejected** (protowire#350), by `Parse` and the decoder, in a `@dataset` cell too. `BindRow` writes a line feed in a cell's string as `\n`, so a cell holding the escape still binds.
- **A singular field or a map key bound twice in one message body is rejected** (protowire#347, #396). A second scalar replaced the first and two blocks merged; now each is an error, whatever the values. **This rejects documents that were accepted before.** A repeated map key also stayed in the message, so a `DynamicMessage` serialized both entries. Map keys compare as values of the key type, so `1:` and `"1":` on an int-keyed map are one key. Repeated fields still concatenate, a map bound twice with distinct keys still merges, and each message body is its own scope.
- **The type entry of a `google.protobuf.Any` block follows the spec** (protowire#354). `Parse` rejects `@type = ...` anywhere but as a block's first entry, and rejects a value that is not a string. The decoder now takes a type entry in a list element and a map value, not only a singular field. `name = { type_url = ... }` no longer fails: an Any block with no type entry is the Any's own fields. An Any body no longer marks a root field of the same name as present in `Result`.
- **`BindRow` and `DatasetReader::Scan` run the bind-time schema checks and apply `(pxf.required)` and `(pxf.default)`.** They used `Unmarshal` with validation off, so a schema the spec refuses was accepted, and an empty cell was neither defaulted nor rejected. `Scan` checks once per row type. `BindRow` checks on every call unless the caller passes `skip_validate`. **A row missing a required field, or a row type that breaks a bind-time rule, is now rejected.**
- **A `BindRow` or `Scan` decode error names its column and reports the row's position,** not a line of the synthetic body.
- **`pxf.BigFloat` text is exact, at any precision** (protowire#393, #406, #410). It went through a `double`, so `Marshal` wrote `inf` from exponent 1024 up, `0` from -1075 down, and misread a mantissa past 8 bytes. It now writes the digits the precision implies, then a suffix: `1.5p53`. A suffixed literal reads at that precision. An unsuffixed one reads at 256 bits, not 53. **Output text changes for every BigFloat, and so does the stored value of an unsuffixed literal.** A suffix on any other field is rejected, as are `p0` and a precision past `p13603`. `MaxNumericLiteralDigits` counts a BigFloat's mantissa digits only.
- **A `pxf.BigFloat` past `MaxBigFloatExponent` (16384) is refused** (protowire#281), reading or writing, before any work. `1e646456992` costs nothing to reject.
- **`@required` and `@default` are honoured** (protowire-go#81). They lower to the annotation carrier at extension 1327, which this port never read, so a schema migrated to that spelling bound with no enforcement and no diagnostic. `IsRequired`, `GetDefault`, `UnmarshalFull` and the bind-time checks now see both spellings. `(pxf.default)` and `@default` with different values on one field, or an `@default` that reduces to no single literal, is the new violation `ViolationKind::kDefaultOption`. **A schema in that spelling now gets the enforcement it declared.** protowire-python inherits this.
- **`ValidateFile` has the rest of the bind-time rules** (draft -01 §annotation-extensions). These are now violations, in the `(pxf.*)` and `@` spellings alike: a default on a repeated, map or group field, or on a message type with no literal; a default on more than one member of a oneof; `(pxf.required)` on a oneof member. **A schema breaking one of these now fails every decode that does not set `skip_validate`.**
- **An option at a retired extension number is diagnosed** (STABILITY.md promise 3), as the new `ViolationKind::kRetiredNumber`. A descriptor compiled before protowire v1.12.0 lost its `(pxf.required)` and `(pxf.default)` silently; it now fails to bind and must be recompiled. The check runs only in a file importing protowire's annotations, on the Options kind each number was allocated on.
- **`UnmarshalFull` with `skip_validate` no longer aborts on a default on a repeated or map field.** It returns an error.
- **`Marshal` leaves out an empty map,** unless `emit_defaults` is set. It wrote `name = {\n}`, which read back as present, so a required map was satisfied by nothing. **Output text changes for a message with an empty map.**
- **`(pxf.required)` and `(pxf.default)` apply in list elements, map values, keyed entries and Any bodies** (protowire-go#161). `UnmarshalFull` checked singular message fields only, so a list element missing a required field was accepted. **This rejects documents that were accepted before, and fills defaults in where nothing was set.**
- **A field in a list element or map value no longer marks its parent's path.** `Result` recorded it at the parent's prefix, so an element's `x` could satisfy a required `x` beside the list. It is now `items[0].x`, or `by_key["k"].x`. A field in an Any body is `payload.x`. **Paths from `Result` change for these fields.**
- **A keyed entry's name counts as supplying the key field** (protowire-go#214, draft -01 §3.13). `kd { alpha {} }` keeps `name: "alpha"` when the key field has a `(pxf.default)`, where the default used to replace the name with no error. A `(pxf.required)` key field is no longer reported absent in the block form, and a `(pxf.default)` on a oneof sibling of the key no longer clears the key. `Result` now reports the key as set (`kd[0].name`). This costs `UnmarshalFull` one presence record per entry; `Unmarshal` is unchanged.
- **A keyed entry's body can't undo its entry name** (draft -01 §3.13, protowire#480, protowire-go#215). Inside a named entry, `null` on the key field and any binding of another member of the key field's oneof are now key conflicts. `choices { alpha { other = "x" } }` lost the key with no error, and `tags { alpha { name = null } }` kept it while reporting it null. **This rejects documents that were accepted before,** on a key field that is optional or in a oneof only. The anonymous form and an agreeing `name = "alpha"` stay legal.
- **`Marshal` writes what protowire-go writes, byte for byte.** **Output text changes for most messages.** A message field is `name { ... }`, not `name = { ... }`. Every list is one element per line, with commas between elements. A float or double has the shortest digits that read back the same, so `0.1` stays `0.1`. A timestamp's fraction loses every trailing zero, so `.5Z`, not `.500Z`. Map entries sort by their written key as text, so `10` comes before `9`. A quoted map key is escaped as `strconv.Quote` does. An enum number with no name is written as the number. An Any in a list element or map value is written as `type_url` and `value`, with no `@type` sugar.
- **Error text matches protowire-go,** for `Parse`, the decoder and the lexer: `expected identifier, string, integer, or bool, got , (",")`, `unexpected character ';'`, `"-" must be followed by a digit or "inf"`, and so on. **Text a caller matched on changes.** One exception: a missing resolver names `UnmarshalOptions::type_resolver`, this port's spelling.
- **A string literal on a bytes field, or a bytes literal on a string field, is rejected,** as in protowire-go. They were converted. **This rejects documents that were accepted before.**
- **A number too large for its field is rejected.** `1e999` on a double read back as `inf`, and `1e39` on a float the same. **This rejects documents that were accepted before.**
- **A timestamp with a day its month lacks is rejected,** rather than rolled forward: `2026-02-30` read as March 2nd. So is `:60` seconds. **This rejects documents that were accepted before.**
- **`inf`, `+inf`, `-inf` and `nan` read on a float or double field,** and `b"..."` takes URL-safe base64 as well as standard. `nan` stores Go's bit pattern.
- **A literal marks the fields it sets in `Result`:** `at = 2026-...` marks `at.seconds` and `at.nanos`, as the block form does. Elements and map values are the unit, and don't.
- **A packed Any holds protowire-go's bytes.** libprotobuf writes fields by number, and Go writes oneof members last, so a packed `value` differed across ports whenever a oneof was set. The new `SerializeGoOrder` writes Go's order.
- **`Marshal` writes `null` at every level** (#78), in list elements, map values, keyed entries and Any bodies. It only wrote top-level ones. A scalar or repeated field named in the set is written as if it were not named.
- **`Marshal` with `emit_defaults` no longer overflows the stack on a message that holds its own type.** Unset message fields stay unwritten, as in protowire-go.
- **A oneof member's default no longer destroys the member the document chose** (draft -01 §annotation-extensions, "Oneof Members"). Applying it cleared the sibling, in a singular message as well as an element.
- **An Any body packs to the same bytes on every decode,** as in protowire-go. Map entries had no fixed order.
- **A null in an Any body is recorded in the `_null` mask** under the Any's path, unless the Any sits in a list element or map value, as in protowire-go.
- **A missing required field's error is positioned at 1:1,** as in protowire-go.
- **A map value takes the literal forms** (a timestamp, duration, wrapper or big number). Only a block was accepted, so a map of these failed to read back what `Marshal` wrote.
- **A bool default is exactly `true` or `false`** (protowire-go#90). Any other spelling silently became `false`; it now fails the decode. **A schema with `(pxf.default) = "True"` now fails `UnmarshalFull`.**
- **`ViolationKindName` and `Violation::ToString` use protowire-go's text:** `keyed field option`, `default field option`, `required field option`, and `invalid (pxf.key) = "..."`.
- **`AppendDatasetRow` writes an empty `(pxf.required)` repeated field or map as `[]` or `{}`** (protowire-go#219). The reader counts both as supplied. This port returned `unsupported kind`, and inside a block cell it left the field out, so the reader refused the row.
- **A PXF `BigFloat` literal whose exponent less its `prec` is below `INT32_MIN` is refused** (#125, protowire-go#231), with Go's `big float literal <s> is below the wire's exponent range`. The wire carries that number as an `int32`, and it used to wrap, so `0.125p2147483647` was stored as a different value. Both exponent checks now run before the value is widened to `prec` bits, so such a literal is refused at once instead of after building a 256 MiB mantissa. Only a `max_numeric_literal_digits` raised past 646 million admits a `prec` that wide.

## [1.2.0] — 2026-10-31

### Fixed

- **`pxf::Marshal` refuses a `pxf.Decimal` past the bounds, and no longer hangs** (protowire#448). A scale near `INT32_MAX` padded one digit at a time, so five input bytes took the encoder effectively forever. A scale past `MaxNumericLiteralDigits` on either sign now fails before any work, as does a `BigInt` or `Decimal` literal past the digit cap. This reaches a message read by a protobuf runtime, which the PB decoder's own bound doesn't cover.
- **A negative `Decimal` scale keeps its value.** It was dropped, so `5` at scale `-2` was written as `5` rather than `500`.

### Changed

- **`FormatBigInt`, `FormatDecimal`, `ReadBigIntAsString` and `ReadDecimalAsString` return `StatusOr<std::string>`,** so they can refuse a value.

### Fixed

- **`(pxf.required)` with `(pxf.default)` on one field is a bind-time
  violation** (#50), as draft -01 now requires (protowire#453).
  `ValidateFile` reports it as `ViolationKind::kRequiredOption`, over the
  import closure, and `Unmarshal` and `UnmarshalFull` refuse such a schema.
  Such a schema used to bind here, and which option won depended on the
  engine. `(pxf.required) = false` beside a default stays accepted. The
  bind-failure `Status` now starts `PXF schema violations:`, as in the Go
  reference, instead of `PXF schema reserved-name violations:`, because
  it no longer lists only reserved names.

- **`check_decode`'s `Tree` has `branches` and `tags`** (protowire#449).
  Without them the PB leg skipped both fields as unknown, so the corpus's
  map-depth rows could not reach the decoder's map path. The decoders
  needed no change: they already count a map's `{` and every PB map entry
  toward `MaxNestingDepth`.
  
### Changed

- **The vendored shared schemas are re-synced with [trendvidia/protos](https://github.com/trendvidia/protos) v0.1.2** ([trendvidia/protowire#447](https://github.com/trendvidia/protowire/issues/447)). They gain protos' file options, and `envelope.proto`'s `go_package` now names a published package. Declarations and field numbers are unchanged, and so is this port's public API.

## [1.1.0] — 2026-09-11

### Added

- **pxf: quoted entry names and keyed repeated fields** (#18; draft `-01`
  §3.13, protowire#116, reference protowire-go#50). The vendored
  `proto/pxf/annotations.proto` gains `(pxf.key) = 1316`, matching
  canonical. The grammar accepts a string at entry-name position with
  `=` and `{` everywhere (`Assignment::key_quoted`, `Block::name_quoted`
  retain the spelling for `FormatDocument`); the schema layer rejects it
  outside a keyed repeated field's block. A repeated message-typed field
  whose `(pxf.key)` names a singular string field of the element message
  decodes from the keyed block form — `children { greeting { … } }` or
  `children = { greeting = { … } }` — with the entry name as the key,
  entry order as list order, and duplicate names, the empty string, and
  a disagreeing explicit key assignment as errors; the anonymous list
  form still decodes, with an explicit empty key rejected. `Marshal`
  writes the keyed form whenever every key is present, non-empty and
  distinct (names bare when identifier-safe, quoted otherwise, the key
  field omitted from the entry body), and the anonymous form otherwise.
  `CanonicalizeKeyed(Document*, const Descriptor*)` (new
  `protowire/pxf/keyed.h`) is the schema-aware AST rewrite behind `fmt`:
  eligible anonymous bindings become keyed blocks, `name = { }` becomes
  `name { }`, identifier-safe quoted names are unquoted, redundant key
  assignments are dropped. `ValidateFile` reports a misplaced
  `(pxf.key)` as `ViolationKind::kKeyOption` with a `detail`;
  `KeyFieldName`, `KeyField` and `IsKeyed` are exported from
  `protowire/pxf/annotations.h`. The spec repo's `testdata/keyed/` is
  vendored and every fixture is pinned.

### Fixed

- **pxf: bind-time validation covers the import closure** (#34; draft
  `-01` § Scope of Bind-Time Checks). `ValidateFile` and
  `ValidateDescriptor` walked the declaring file only, so a field named
  `null` or a misplaced `(pxf.key)` in an imported `.proto` bound
  without a violation and was silently unhonoured when the decoder
  reached it. They now walk the transitive imports, each file once
  (the diamond A → B, C → D reports D once), and `Violation::file`
  names the declaring file; results sort by file, then element.
  `google/protobuf/*` files are skipped in the walk — they carry no PXF
  annotation and no reserved name, pinned by a test that validates each
  of them directly — because descriptor.proto sits in the closure of
  every annotated schema and walking it measured 1.1 µs per decode, a
  quarter of a small document's decode; the walk now costs ~160 ns.
  `UnmarshalOptions::skip_validate` remains the escape hatch for callers
  that validated once.

- **pb: repeated numeric fields are marshalled packed** (#32). `Marshal`
  wrote one tag + value per element for every element type, so
  `ListHolder { values: [1, 2, 3] }` was `08 01 08 02 08 03` where the
  reference's `pb.Marshal` and every protoc-generated encoder write the
  proto3 default `0a 03 01 02 03`. Bool, integer (zigzag honoured) and
  float element types are now one LEN record of concatenated element
  encodings, every element emitted, zeros included; strings, bytes and
  messages stay one record per element, and an empty repeated field
  writes nothing. The decoder has accepted both forms since #31; the
  canonical envelope has no repeated numerics, so its bytes are
  unchanged.

- **pxf: a dotted string map key is written bare** (#36; protowire#313,
  decided as (a)). The identifier-safe test `Marshal` and
  `FormatDocument` apply to a string map key stopped at `[A-Za-z0-9_]`,
  while the grammar's *ident-part* — and the test keyed entry names use
  — admits `.`, so a key `a.b` was marshalled `"a.b":` and a quoted
  `"a.b"` kept its quotes through fmt, where the text says bare.
  `IsIdentifierSafe` is `IsIdentifierSafeEntryName` now: one
  identifier-safe rule for the document. `".e"` and `"1.5"` still fail
  *ident-start* and stay quoted. The spec's third fmt pair,
  `fmt-dotted-keys`, is vendored and pinned; the decoder always read
  `a.b:` as one identifier, so only the writers move.

- **pb: a map entry always carries key and value, zero-valued or not**
  (#24; protowire#295, decided on protowire-go#105). `pb.h`'s map branch
  applied proto3 zero-skip inside the entry, so `metadata { "": "" }`
  serialised as an empty entry (`22022a00`) where protobuf-go, protoc and
  C++ protobuf write `22062a040a001200`. Field 1 and field 2 of every
  entry are now written unconditionally; a value held through
  `std::optional` or a smart pointer that is unset is written as its zero
  value. `dump_envelope --vector zero-map-entry` prints the bytes for the
  cross-port gate's golden, and an unknown vector name exits 3 with
  `not-implemented: <name>`.

- **pxf fmt: the quotes on a string map key spelled like a keyword or an
  integer are kept** (#27; draft `-01` § Entries and Keys, "Canonical
  spelling of map keys", protowire#306). `FormatDocument` wrote every
  identifier-shaped key bare, so `"true": "v"` on a `map<string, V>`
  became `true: "v"` — a bool key, which no longer binds on a string
  `K`. `MapEntry` gains `key_quoted`; a quoted key is unquoted only when
  it is identifier-safe and not `null` / `true` / `false`, and a bare key
  stays bare (so `404:` is no longer requoted on the way through fmt).
  The marshaller uses the same test (`IsIdentifierSafe`, exported from
  `protowire/pxf/format.h`), so `"123"`, `"true"` and `"null"` string
  keys are written quoted; it also sorts bool keys (false, true). The
  spec repo's `testdata/map-keys/` fixtures are vendored and pinned.

- **pxf: bool map keys bind in exactly the grammar's spellings**
  (absorbed into #27; protowire#284). The decoder bound any non-`true`
  key on a `map<bool, V>` to `false` — `t`, `yes`, `"TRUE"`, `"0"` all
  silently became a key the author did not write. A bool key is now
  `true` / `false` bare (the keyword spelling, newly accepted as a map
  key by the parser and decoder), `0` / `1` bare, or `"true"` /
  `"false"` quoted, and anything else is an error naming the key and
  field; on a string `K` the bare keyword is an error that says to
  write it quoted.

- **pxf: the lexer reads fractional and `µs` duration literals** (#20).
  `1.5ms`, `1.234567ms`, `312.5µs`, `1h30m0.5s` and `-2.5s` — the forms
  every port's encoder writes for a `google.protobuf.Duration` that is
  not a whole multiple of its largest unit — tokenised as a float
  followed by an identifier (or an integer followed by a stray byte for
  the two-byte `µ`), so this port could not read its own output for any
  measured latency. `LexNumber` now consumes an optional fraction first
  and takes the duration branch when a time unit follows; the duration
  scan admits `.` before a digit and the `C2 B5` micro sign (U+00B5
  only — U+03BC is not in the grammar). Mirrors protowire-go#76.

- **detail: negative durations split toward zero, and the int64 edges
  round-trip** (absorbed into #20). `ParseDuration` normalised nanos into
  `[0, 1e9)`, so `-1ns` decoded to `seconds=-1, nanos=999999999`, which
  `google.protobuf.Duration` forbids (nanos must carry the sign of the
  value) and no other port reads back as `-1ns`; it now splits like
  `time.Duration`. `FormatDuration` and `ParseDuration` take the
  magnitude in unsigned arithmetic, so `INT64_MIN` formats as
  `-2562047h47m16.854775808s` instead of a placeholder and reads back.

### Security

- **HARDENING.md § Mandatory limits are enforced, configurable per
  call, and the adversarial corpus passes on every row** (#26, #25;
  draft `-01` § Mandatory Limits, protowire#299, #301). Measured on
  `main` at 2a8c854 with the spec repo's `cross_security_check.sh`, ten
  of the port's rows failed: no PXF depth cap at all (200 and 1000
  levels accepted, 100 000 levels a stack overflow, 101 levels of blocks
  or lists accepted), a `\xFF\xFE` escape accepted into a proto3
  string, a 5000-digit literal accepted on a `pxf.BigInt`, and the
  three SBE rows reported as crashes. **Cause:** never wired, not a
  regression — no commit in this repository's history references
  `MaxNestingDepth`, `check_decode` (#8) exited 2 for `--format sbe`
  ("not implemented"), which the harness classifies as a crash, and its
  PB leg went through libprotobuf's parser rather than this port's
  codec. The gaps #1 recorded at M8 were closed without landing.
  - `pxf::UnmarshalOptions` gains `max_message_size`,
    `max_nesting_depth`, `max_numeric_literal_digits`,
    `max_bytes_literal_length` and `max_repeated_count` (0 = the default
    in the new `protowire/limits.h`); `Parse` takes a `ParseOptions`
    with the first, second and fourth. The input size is checked before
    the first token; every `{` or `[` is one descent from a root at 0,
    counted identically by `Parse` and `Unmarshal`, so exactly 100
    descents decode and 101 do not; a `b"…"` literal is refused from its
    length before it is decoded; a repeated or map field is refused
    before its element past the bound is allocated; a literal bound to
    `pxf.BigInt` / `Decimal` / `BigFloat` is refused past 4096 digits;
    a proto3 `string` field (scalar, repeated, map key) refuses invalid
    UTF-8 from `\xHH` / `\NNN` escapes or raw bytes, while `bytes` fields
    take them. The decoder now surfaces the lexer's own diagnostic for an
    ILLEGAL token instead of "expected string".
  - `pb::Unmarshal` takes a `pb::UnmarshalOptions` (`max_message_size`,
    `max_nesting_depth`, `max_numeric_literal_digits`,
    `max_repeated_count`). The depth counter is threaded through nested
    submessages, map entries and big-number messages rather than reset
    by the fresh span; `pxf.Decimal.scale` is bounded to ±4096 on the
    wire (protowire#279); and repeated numeric fields decode from the
    **packed** form every other encoder in the family writes — before,
    `0a 03 01 02 03` read as one element and corrupt tags.
  - `sbe::Codec::New` takes a `sbe::CodecOptions` (`max_message_size`,
    `max_repeated_count`). `Unmarshal` and `NewView` validate the header
    (template id, wire block ≥ template block) and every group header
    before any entry is allocated: wire entry block ≥ template's, no
    zero-length block with a non-zero count, count × block within the
    remaining input (as a division, so it cannot overflow), count within
    `MaxRepeatedCount`. `GroupView::Entry` past the count reads as an
    empty view instead of a span past the buffer. A `char[]` decoded into
    a proto3 string refuses invalid UTF-8.
  - `check_decode` gains `--limit NAME=VALUE`, decodes `--format sbe`
    through the codec, and `--format pb` through this port's codec with
    hand-mirrored `adversarial.proto` types, as the Go reference does.
    All 38 (port, corpus) pairs pass, including the twelve `limits` rows
    and the three `pb/decimal-*` rows the manifest skipped for this
    port.

## [1.0.0] — 2026-05-13

First major-version cut. Implements the three one-time spec changes
from the protowire v1.0 freeze line (`STABILITY.md` in the spec
repo) in lockstep with `protowire`, `protowire-go`, `protowire-java`
(v1.0.1), `protowire-typescript`, and `protowire-kotlin`.
**Breaking** — there is no alias period; v1.0 is itself the major
bump.

### v1.0 spec changes

- **`@table` → `@dataset` rename** (draft §3.4.4). Public API
  follows: `Ast::TableDirective` → `Ast::DatasetDirective`,
  `Ast::TableRow` → `Ast::DatasetRow`, `TokenKind::kAtTable` →
  `TokenKind::kAtDataset`, `Result::Tables()` → `Result::Datasets()`,
  `Result::AddTable()` → `Result::AddDataset()`,
  `class TableReader` → `class DatasetReader`. Headers
  `protowire/pxf/table_reader.h` → `dataset_reader.h`; source
  `src/pxf/table_reader.cc` → `dataset_reader.cc`. Hard cutover.

- **`@proto` directive added** (draft §3.4.5). New `Ast::ProtoDirective`
  struct + `Ast::ProtoShape` enum (`kAnonymous`, `kNamed`, `kSource`,
  `kDescriptor`). Four body shapes lexically distinguished
  (anonymous `{ ... }`, named `<dotted-name> { ... }`,
  source `"""..."""`, descriptor `b"..."`). Exposed via
  `Document::protos` and `Result::Protos()`. Descriptor form is
  the MUST-support shape per spec; this port supports all four.

- **Reserved directive names** expanded from 5 to 13 (draft §3.4.6).
  `IsFutureReservedDirective(name)` exposed from
  `protowire/pxf/schema.h`. Parser + fast decoder reject `@table`,
  `@datasource`, `@view`, `@procedure`, `@function`,
  `@permissions` as spec-reserved.

`@dataset`'s row message type is now optional in the AST — binding
to an anonymous `@proto` per draft §3.4.4 Anonymous binding.

`Lexer::RepositionTo(int)` added so the parser can skip past an
`@proto` brace-body whose interior is protobuf source rather than
PXF.

### Build

- CMake `project(protowire VERSION ...)` bumped `0.75.0` → `1.0.0`.

### Tests

- New `test/pxf_proto_directive_test.cc` with 11 cases covering all
  four `@proto` body shapes, anonymous binding, multi-`@proto`,
  nested-brace bodies, reserved-name rejection, `@type` coexistence.
- `ctest`: 229 tests, 0 failures.

## [0.75.0] — 2026-05-12

First release after the v0.70.0 baseline that closes the v0.72–v0.75
gap with the rest of the `protowire-*` stack (Go, Java). All four PXF
v0.72-series features are now available in the C++ port, in lockstep
with what the Go and Java ports shipped over their v0.72 → v0.74 →
v0.75 cuts. The cpp port skips intermediate version numbers and lands
the bundled feature set directly on v0.75.0 to match the active wire
revision.

### Added

- **`TableReader` streaming `@table` consumption + `Scan` / `BindRow`
  per-row binding** (draft §3.4.4). `UnmarshalFull` materializes
  every row of an `@table` directive into `Result::Tables()`; that
  works for small datasets and breaks for the CSV-replacement
  workload `@table` was designed for. New
  `<protowire/pxf/table_reader.h>` exposes:
  - `TableReader::Create(std::istream*)` — consumes any leading
    directives and the `@table TYPE ( cols )` header, returns a
    reader positioned at the first row. Header is capped at 64 KiB
    (`kDefaultHeaderMaxBytes`) to fail-fast when a non-`@table`
    document is handed in by mistake.
  - `Type()` / `Columns()` / `Directives()` accessors.
  - `Next(TableRow*)` pulls one row at a time from the underlying
    stream; working-set memory is bounded by the largest single row,
    not the full table. Per-row arity and v1 cell-grammar checks
    happen at consume time (not deferred to EOF), matching the
    spec's streaming-consumer requirements.
  - `Scan(Message*)` — convenience: `Next` + `BindRow`.
  - `Tail()` — returns the unconsumed buffer plus the remaining
    underlying source as a fresh `std::istream`, so callers can chain
    a second `Create()` for documents with multiple `@table`
    directives.
  - `BindRow(Message*, columns, row)` — exported helper for callers
    iterating `Result::Tables()[i].rows` from the materializing
    path. Strategy is format-and-reparse: render the row as a
    synthetic PXF body (`<col> = <val>` per non-`std::nullopt` cell)
    and run it through `Unmarshal`. This reuses every branch of the
    existing decoder — WKT timestamps / durations, wrapper
    nullability, enum-by-name resolution, `pxf.required` /
    `pxf.default`, oneof handling — instead of growing a parallel
    Value→FieldDescriptor switch. `SkipValidate` avoids re-running
    the reserved-name check per row.

- **`Result::Directives()` and `Result::Tables()` accessors.** The
  fast-path direct decoder now populates the document-root directive
  list and `@table` directive list on `Result` during
  `UnmarshalFull`, so consumers can read them after a decode call.
  - `Result::Directives()` returns the generic
    `@<name> *(prefix) [{ ... }]` blocks in source order, with raw
    body bytes preserved verbatim for downstream re-parsing
    (chameleon's `@header T { ... }` reader, etc.). A single prefix
    populates the back-compat `type` field; two or more leave it
    empty and consumers read `prefixes[]` directly.
  - `Result::Tables()` returns the `@table` directives with full
    column metadata and parsed cell values per row, faithful to the
    three-state cell grammar (absent / present-but-null /
    present-with-value).
  - `Unmarshal` (vs `UnmarshalFull`) still passes a null Result and
    walks directives without allocating any AST nodes — the fast
    path retains its zero-allocation contract on the hot path.


- **PXF schema reserved-name validator (`SchemaValidator`, draft §3.13).**
  Rejects protobuf schemas that declare a message field, oneof, or
  enum value whose name is case-sensitively equal to a PXF value
  keyword (`null` / `true` / `false`) — such a name lexes as the
  keyword and the declared element is unreachable from PXF surface
  syntax. New `<protowire/pxf/schema.h>` exposes `ValidateDescriptor`,
  `ValidateFile`, and the `Violation` struct (with `file`, `element`,
  `name`, `kind`); results are sorted by element FQN for stable
  output. `UnmarshalOptions` gains `skip_validate` for consumers that
  validate once at registry-load time and don't want the per-call
  recheck cost. `Unmarshal` and `UnmarshalFull` invoke the validator
  on the message's descriptor before decode runs; violations come
  back as a joined-string error message with one line per offender.


- **PXF parser-side `@<name>` / `@entry` / `@table` directive grammar**
  (draft §3.4.2 – §3.4.4). The AST `Document` now carries
  `directives` (generic `@<name> *(prefix) [{ ... }]` entries) and
  `tables` (`@table <type> ( cols ) row*` entries) alongside the
  existing `type_url` and `entries`. `Directive::body` preserves the
  raw bytes between `{` and `}`; `Directive::type` keeps the legacy
  single-prefix shape for v0.72.0-era consumers. `Document::body_offset`
  marks the byte right after the last directive (used by chameleon
  for hashing the schema-typed payload).
  Both the AST parser and the fast direct-decode path consume the new
  forms; runtime semantics (Result accessors, TableReader streaming,
  per-row Scan/BindRow) follow in subsequent PRs of the v0.72-v0.75
  catch-up sequence. The fast path discards directive contents for
  now and enforces the standalone constraint: a document containing
  any `@table` directive MUST NOT also carry `@type` or top-level
  field entries (draft §3.4.4).
  `Position` gains a `offset` field (byte offset into the lexer's
  input) so directive Body extraction can slice raw bytes; existing
  callers that read only line/column are unaffected.


- **`cmd/check_decode` HARDENING conformance binary.** The per-port
  binary the spec repo's `scripts/cross_security_check.sh` expects
  for every C++/Go/Java/etc. port. Runtime-compiles a `--proto` via
  libprotoc's `Importer`, decodes a `--input` against a `--schema`
  descriptor, and exits 0 on accept / 1 on clean reject / >1 on
  crash. Mirrors the Go reference at
  `protowire-go/scripts/check_decode/main.go`.

  Supported formats:
  - `pxf` — `protowire::pxf::Unmarshal` against a `DynamicMessage`
    built from the runtime-compiled descriptor.
  - `pb` — standard libprotobuf `Message::ParseFromArray` against a
    `DynamicMessage`. The C++ port's `protowire::pb` codec is
    struct-tag-driven and doesn't accept descriptor-bound inputs,
    so the descriptor-driven hardening tier exercises libprotobuf's
    parser primitives directly — which is what the adversarial
    corpus's depth / length / overflow probes actually hit on any
    libprotobuf-backed consumer.
  - `sbe`, `envelope` — not yet implemented; returns exit 2
    (configuration error) so the manifest can mark per-port skips.

  Build is gated on `libprotoc` being available — Ubuntu ships it
  as `libprotoc-dev`, separate from `libprotobuf-dev`. Consumers
  without `libprotoc` get a CMake `STATUS` message and the target
  is skipped; the rest of the build is unaffected.

  Surfaces real port-level HARDENING gaps. Smoke-running against
  the spec repo's adversarial corpus on `main` already shows:
  - `deep-nesting-200.pxf` accepted (cpp has no MaxNestingDepth cap)
  - `invalid-utf8-string.pxf` accepted (cpp has no proto3 UTF-8
    enforcement)

  Those are real HARDENING.md violations the cross-port harness now
  catches automatically. Tracked as a follow-up; this PR only adds
  the binary and wires the build.

## [0.70.0]

Initial public release. The version number aligns this port with the rest
of the `protowire-*` stack, which targets the 0.70.x series for the first
coordinated public release. The Python port (`protowire-python`) consumes
this library through the nanobind FFI and inherits its behaviour.

### Added

- **CMake `find_package` install** under `protowire::` namespace. Consumers
  can `find_package(protowire CONFIG REQUIRED)` after `cmake --install`
  and link against `protowire::pxf`, `protowire::sbe`,
  `protowire::envelope`, `protowire::pb`, `protowire::detail`,
  `protowire::protos`. Transitively pulls in `protobuf::libprotobuf`.
- **Comprehensive CI matrix**: Linux × {gcc-13, clang-18}, macOS ×
  AppleClang, Windows × MSVC (with vcpkg for protobuf), plus a
  dedicated AddressSanitizer + UBSan job that runs the full test suite
  under sanitizers on every PR. clang-format + clang-tidy gates,
  llvm-cov coverage uploaded to Codecov, weekly CodeQL SAST.
- **Governance scaffolding**: `LICENSE` (MIT), `CONTRIBUTING.md`,
  `SECURITY.md` (security@trendvidia.com), `GOVERNANCE.md`,
  `CODE_OF_CONDUCT.md`, `.github/CODEOWNERS`, issue + PR templates,
  Dependabot for GitHub Actions.
- **Style configs**: root `.clang-format` and `.clang-tidy` matching the
  in-tree style; CI's clang-format check job rejects unformatted code.

### Changed (breaking)

- **PXF parser stricter on key forms**, mirroring the upstream grammar
  tightening in
  [`trendvidia/protowire@8262bbb`](https://github.com/trendvidia/protowire/commit/8262bbb)
  (`docs/grammar.ebnf`, `docs/draft-trendvidia-protowire-00.txt`):
  - `=` (field assignment) and `{ … }` (submessage) now require an
    identifier key. Inputs like `123 = 234` or `child { 123 = 123 }`
    are now parse errors with
    `"field assignment with '=' requires an identifier key, got integer
    (\"123\"); use ':' for map entries"`.
  - `:` (map entry) is rejected at document top level — the document
    represents a proto message, never a `map<K,V>`. Use `=` for
    top-level field assignments. Map literals (`field = { 1: "x" }`)
    still work because `:` remains valid inside `{ … }` blocks.
