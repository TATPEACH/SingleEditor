# Offline ETL raw decoder, contract version 1

ToolTestOnly. `SingleEditorEtlDecode.exe input.etl new-output-directory` reads one
existing ETL with Windows `OpenTraceW` / `ProcessTrace` and local TDH metadata. It
does not start/stop/alter a session, load a product, retrieve remote symbols, or
classify a file event as a read. Both arguments are normalized absolute paths.
The output directory must not exist and its parent must exist. Input remains
leased read-only (no write/delete sharing), and SHA256/size/path are checked on
both sides of decoding. Output failures retain partial evidence.

Exit 0 means raw decoding finished without reported structural gaps; 3 means it
finished with typed gaps; 1 means overall failure. None is a release pass.
An input-open/output-directory failure occurs before a receipt can be written.

## Files and exact fields

`header.json`: `schema: singleeditor.etl-decoder`, `version: 1`.

* `status`: `decoded | decoded_with_gaps | failed`.
* `input`, `input_after`, `decoder`, `events`: actual `{path,bytes,sha256}` facts,
  or null if unavailable on failure. `events` identifies `events.jsonl`.
* `input_unchanged`: equality of the complete non-null input facts.
* `process_trace_mode`: **268439552** (`0x10001000`, EVENT_RECORD | RAW_TIMESTAMP).
* `session`: null if OpenTrace failed, otherwise:
  `clock_type` (raw `TRACE_LOGFILE_HEADER.ReservedFlags`), `perf_frequency`,
  `start_time`, `end_time`, `boot_time`, `events_lost`, `buffers_lost`,
  `buffers_written`, `pointer_size`, `log_file_mode`, `os_version`,
  `provider_version`, `processor_count`.
  The four time/frequency values are decimal strings. Start/end/boot are raw
  header FILETIME values, not QPC ticks; `timestamp_raw` below is the session's
  original clock. Only `clock_type == 1` plus positive frequency permits QPC
  correlation. Other clocks are retained with `non_qpc_clock`, never converted.
* `open_trace_status`, `process_trace_status`, `close_trace_status`: Win32 return
  values; unattempted process/close use ERROR_INVALID_STATE (5023).
* `buffers_read`, `event_count`, `gap_event_count`, `gaps`, `error` (string/null).
* `release_eligible`: always false. `scope` explicitly denies coverage or absence
  inference. Actual capture filters must be verified from the external receipt
  and profile; the reader cannot discover System/other-process coverage here.

`events.jsonl`: one JSON object per actual EVENT_RECORD callback, no prefilter:

* `schema: singleeditor.etl-event`, `version: 1`, `sequence` (one-based),
  `pid`, `tid`, `timestamp_raw` (signed decimal string), `provider` (uppercase
  GUID without braces), `descriptor: {id,version,channel,level,opcode,task,keyword}`.
  `keyword` is `0x` plus sixteen uppercase hex digits.
* `header_flags`, `header_properties`, `activity` (GUID), `payload_bytes`,
  `payload_hex` (complete original UserData, uppercase hexadecimal).
* `extended_data_count`, `extended_data: [{type,linkage,bytes,raw_hex}]`.
  Extended items remain raw and cause `extended_data_not_interpreted`.
* `tdh`: always `api`, `status` (TdhGetEventInformation return), `metadata_bytes`,
  and `properties`. Successful bounded metadata additionally supplies
  `decoding_source` (SDK DECODING_SOURCE), `provider_guid`, `descriptor`,
  `provider_name`, `event_name`, `task_name`, `opcode_name`, `property_count`,
  `top_level_property_count`, `pointer_size`, and when property parsing starts,
  `decoded_payload_bytes`. Fields unavailable because of a gap are omitted.
* `tdh.properties`: flat scalar/fixed-array/prior-unsigned-count fields actually
  decoded by TdhFormatProperty, in source order. Each has `index`, `array_index`,
  `name`, `flags`, `in_type`, `out_type`, `map_name`, `map_applied: false`,
  `declared_length`, `declared_count`, `offset`, `tdh_status`,
  `formatted_buffer_bytes`, `bytes`, `raw_hex`, `formatted`. The latter three
  are null on a failed formatting attempt. `bytes` is TDH UserDataConsumed;
  `offset` is within original UserData. No localized formatted value is an
  event semantic or proof of a numeric field; consumers should whitelist exact
  provider/descriptor/version plus actual TDH types and decode raw bytes.
* `gaps`: typed `{code,win32_status,property_index}` objects; the latter is null
  for event/session-wide gaps. Unknown metadata/version, complex structures,
  WPP/string-only/custom/XML fields, unresolved count/length, explicit zero-length
  counted strings, failed formatting or trailing bytes are preserved as gaps,
  with full original UserData retained. There is no classic-kernel opcode map.

No decoded property is claimed when TDH does not supply the exact event metadata.
Arrays and lengths use only preceding UINT8/16/32 scalars; unsupported cases stop
property interpretation for that record rather than guessing its next offset.
TDH metadata and individual formatted values are bounded at 1 MiB.

## Independent build and bounded checks

Run `build-and-check.ps1 -OutputDirectory <new-dir>` in ordinary Windows
PowerShell. It builds two independent ToolTestOnly targets with MSBuild `/m:2`:
the decoder and export contract checks. No product/JUCE CMake is included.
Checks cover escaping, lossless 64-bit decimals, non-QPC preservation, unknown
TDH metadata/raw payload retention, a synthetic payload decoded with the actual
installed Kernel-File NameCreate v0 TDH metadata (no observed file operation),
JSONL parsing, actual OpenTrace rejection of
an invalid file with unchanged source hashes, and preservation of old outputs.
Synthetic records/invalid bytes are explicitly not real ETL or runtime proof.

`checks5/receipt.json` records the passing focused checks. Its build completed
with no warnings/errors; `checks5/contract.jsonl` contains the two explicitly
synthetic event rows and test receipt. `checks5/invalid-decode/header.json` is an
actual failed OpenTrace result (ERROR_FILE_CORRUPT 1392), not a real trace.
Prior outputs remain: checks1 native-stderr PowerShell wrapper issue, checks2 C++
ADL name collision, checks3 incorrect test/document mode constant (reader used
the correct SDK macros), checks4 overly narrow synthetic metadata assertion.

`checks7/receipt.json` freezes the final actual compiler-input facts
before and after its successful ToolTestOnly build: CMakeLists, decoder C++, test
C++, and build script. It binds the actual EXE, CMake executable, configure/build
logs and check output hashes. A consumer must bind the selected EXE to these
retained compiled-source facts, not trust `header.decoder` self-report alone.
The final delivery manifest records these facts; only source/scripts are proposed
for production integration, never the EXE into a shipping payload.
`checks6` retains the initial provenance result. The final source also excludes
explicit count/fixed-count arrays from prior unsigned scalar length/count inputs,
including arrays with one element, as identified by the bounded source review.

The bounded project search of `out/startup-trace-20260918` and
`out/startup-resources-20260917` found no `.etl`. Real project ETL decoding remains
unverified until an existing authorized capture becomes available. Do not collect
a new trace to claim this gap closed, and do not inspect unrelated sessions.

## API authority

Implemented against installed SDK 10.0.26100.0 `shared/evntrace.h`,
`um/evntcons.h`, `um/tdh.h`, and official primary documentation:

* [EVENT_TRACE_LOGFILEW and raw mode](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_logfilew)
* [TRACE_LOGFILE_HEADER clocks and loss fields](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-trace_logfile_header)
* [TdhFormatProperty](https://learn.microsoft.com/en-us/windows/win32/api/tdh/nf-tdh-tdhformatproperty)
* [Microsoft TDH usage example](https://learn.microsoft.com/en-us/windows/win32/etw/using-tdhformatproperty-to-consume-event-data)
