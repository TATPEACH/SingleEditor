// Offline observation only. No trace collection, GUI launch, timing gate or release approval.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const self = fileURLToPath(import.meta.url);
const requireThat = (ok, code, detail = '') => { if (!ok) throw new Error(`${code}: ${detail}`); };
const read = p => JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
const hash = b => crypto.createHash('sha256').update(b).digest('hex').toUpperCase();
const canonical = x => Array.isArray(x) ? x.map(canonical) : x && typeof x === 'object'
    ? Object.fromEntries(Object.keys(x).sort().map(k => [k, canonical(x[k])])) : x;
const equal = (a, b) => JSON.stringify(canonical(a)) === JSON.stringify(canonical(b));
const samePath = (a, b) => typeof a === 'string' && typeof b === 'string'
    && path.resolve(a).toLowerCase() === path.resolve(b).toLowerCase();
const fact = p => ({ path: path.resolve(p), bytes: fs.statSync(p).size, sha256: hash(fs.readFileSync(p)) });
const uint = x => Number.isSafeInteger(x) && x >= 0;
const decimal = (x, code, positive = false) => {
    requireThat(typeof x === 'string' && /^(0|[1-9][0-9]*)$/.test(x), code);
    const n = BigInt(x); requireThat(!positive || n > 0n, code); return n;
};
const RAW_EVENT_RECORD_MODE = 0x10001000; // Installed SDK evntcons.h, not EVENT_RECORD flag 0x100.
const PROCESS = '22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716';
const FILE = 'EDD08927-9CC4-4E65-B970-C2560FB5C289';
const guid = x => typeof x === 'string' && /^[A-F0-9]{8}(?:-[A-F0-9]{4}){3}-[A-F0-9]{12}$/.test(x);
const gap = (code, detail = null) => ({ code, detail });
const existingFact = (expected, label, optionalMissing = false) => {
    requireThat(expected && path.isAbsolute(expected.path) && uint(expected.bytes)
        && /^[A-F0-9]{64}$/.test(expected.sha256), 'trace_file_fact', label);
    if (!fs.existsSync(expected.path) && optionalMissing) return null;
    const actual = fact(expected.path);
    requireThat(samePath(expected.path, actual.path) && expected.bytes === actual.bytes
        && expected.sha256 === actual.sha256, 'trace_input_changed', label);
    return actual;
};
const afterFacts = facts => { for (const f of facts) existingFact(f, f.path); };
const write = (p, x) => fs.writeFileSync(p, JSON.stringify(x, null, 2) + '\n', { flag: 'wx' });

function decoderBuild(receiptPath) {
    const receipt = fact(receiptPath), b = read(receiptPath);
    requireThat(b.schema === 'singleeditor.etl-decoder-focused-checks' && b.version === 1 && b.status === 'pass'
        && b.inputs_unchanged === true && equal(b.inputs, b.inputs_after) && b.inputs?.length === 4
        && b.architecture === 'x64' && b.configuration === 'Release' && equal(b.msbuild_arguments, ['/m:2']),
        'trace_decoder_build_contract');
    const names = ['CMakeLists.txt', 'SingleEditorEtlDecode.cpp', 'DecoderContractTests.cpp', 'build-and-check.ps1'];
    const files = [receipt], currentInputs = [];
    for (const name of names) {
        const matches = b.inputs.filter(f => path.basename(f.path) === name);
        requireThat(matches.length === 1, 'trace_decoder_build_inputs', name);
        const original = existingFact(matches[0], 'decoder_build_' + name);
        const current = fact(path.join(path.dirname(self), 'etw', name));
        requireThat(original.sha256 === current.sha256 && original.bytes === current.bytes, 'trace_decoder_source_changed', name);
        files.push(original); currentInputs.push(current);
    }
    for (const key of ['decoder', 'cmake', 'configure_log', 'build_log', 'contract_output', 'invalid_etl_header'])
        files.push(existingFact(b[key], 'decoder_build_' + key));
    requireThat(b.decoder_sha256 === b.decoder.sha256, 'trace_decoder_build_identity');
    return { receipt, decoder: b.decoder, files, current_inputs: currentInputs };
}

// Separately testable raw Core contract. Synthetic callers cannot create analysis receipts.
export function validateTraceCore(core, candidate, traceClock, commands) {
    requireThat(core.schema === 'singleeditor.core-open.receipt' && core.version === 1
        && core.run_contract?.profile === 'smoke' && core.run_contract.gate_eligible === false
        && core.run_contract.measurement_mode === 'handshake' && core.verdict?.status === 'pass'
        && core.execution?.completed === true && core.execution.settings?.restored === true
        && core.artifacts?.length === 1 && core.samples?.length === 1, 'trace_core_contract');
    const artifact = core.artifacts[0], s = core.samples[0], c = s.clock;
    requireThat(artifact.role === 'candidate' && samePath(artifact.path, candidate.path)
        && artifact.sha256 === candidate.sha256 && artifact.bytes === candidate.bytes, 'trace_core_identity');
    requireThat(s.artifact_role === 'candidate' && s.valid === true && s.invalid_reasons?.length === 0
        && s.measurement_mode === 'handshake' && s.mapping_present === true && uint(s.process_id) && s.process_id > 0
        && s.termination === 'closed-main-window' && s.cleanup?.forced === false && s.cleanup.exit_code === 0,
        'trace_core_sample');
    requireThat(traceClock?.source === 'windows-qpc' && traceClock.integer_encoding === 'decimal-string'
        && c?.source === 'windows-qpc' && c.integer_encoding === 'decimal-string', 'trace_qpc_encoding');
    const frequency = decimal(c.frequency, 'trace_qpc_frequency', true);
    requireThat(frequency === decimal(traceClock.frequency, 'trace_qpc_frequency', true), 'trace_qpc_frequency_mismatch');
    const launch = decimal(c.launch, 'trace_launch_qpc', true);
    const wave = decimal(c.milestones?.first_waveform, 'trace_wave_qpc', true);
    const play = decimal(c.milestones?.playable, 'trace_play_qpc', true);
    const ready = decimal(c.core_ready, 'trace_core_qpc', true);
    requireThat(wave >= launch && play >= launch && ready === (wave > play ? wave : play), 'trace_core_ready_maximum');
    for (const [name, tick] of Object.entries(c.milestones)) {
        if (tick === null) continue;
        const n = decimal(tick, 'trace_milestone_qpc', true);
        requireThat(n >= launch, 'trace_milestone_before_launch', name);
        const value = s.metrics_ms?.[name], elapsed = Number(n - launch) * 1000 / Number(frequency);
        requireThat(Number.isFinite(value) && Math.abs(value - elapsed) <= 0.000501, 'trace_milestone_metric', name);
    }
    requireThat(s.metrics_ms.core_ready === Math.max(s.metrics_ms.first_waveform, s.metrics_ms.playable)
        && Math.abs(s.metrics_ms.core_ready - Number(ready - launch) * 1000 / Number(frequency)) <= 0.000501,
        'trace_core_metric_maximum');
    const starts = commands.filter(x => x.step === 'start' && x.exit_code === 0);
    const stops = commands.filter(x => ['stop', 'stop-incomplete'].includes(x.step) && x.exit_code === 0);
    requireThat(starts.length === 1 && stops.length === 1, 'trace_collection_bounds');
    const begin = decimal(starts[0].end_qpc, 'trace_start_end_qpc', true);
    const end = decimal(stops[0].begin_qpc, 'trace_stop_begin_qpc', true);
    requireThat(begin <= launch && ready <= end && end <= decimal(stops[0].end_qpc, 'trace_stop_end_qpc', true),
        'trace_qpc_interval');
    return { process_id: s.process_id, frequency: c.frequency, launch: c.launch,
        first_waveform: c.milestones.first_waveform, playable: c.milestones.playable, core_ready: c.core_ready,
        trace_started: starts[0].end_qpc, trace_stop_begin: stops[0].begin_qpc,
        trace_stop_end: stops[0].end_qpc };
}

function captureInputs(receiptPath, sourceRoot, selectedGui) {
    const captureFact = fact(receiptPath), t = read(receiptPath), inputs = [captureFact], pending = [];
    requireThat(t.schema === 'singleeditor.startup-trace' && t.version === 1
        && ['captured_pending_analysis', 'failed'].includes(t.status)
        && t.release_eligible === false && t.performance_eligible === false
        && t.collection_mode === 'bounded-memory' && t.buffer_bytes === 16777216,
        'trace_capture_schema');
    requireThat(typeof t.instance === 'string' && /^SingleEditorStartup_[0-9a-f]{32}$/.test(t.instance)
        && Array.isArray(t.commands), 'trace_instance');
    requireThat(t.clock?.source === 'windows-qpc' && t.clock.integer_encoding === 'decimal-string', 'trace_capture_clock');
    decimal(t.clock.frequency, 'trace_capture_frequency', true);
    const candidate = existingFact(t.candidate_after, 'candidate_after'); inputs.push(candidate);
    if (t.candidate) {
        inputs.push(existingFact(t.candidate, 'candidate'));
        requireThat(equal(t.candidate, t.candidate_after), 'trace_candidate_changed');
    }
    requireThat(!selectedGui || samePath(candidate.path, selectedGui), 'trace_selected_gui_location');
    requireThat(t.process_exe_filter === path.basename(candidate.path), 'trace_process_filter');
    for (const [key, relative] of Object.entries({ collector: 'tools/release/Invoke-StartupTrace.ps1',
        runner: 'tools/Invoke-CoreOpenBenchmark.ps1', profile_template: 'tools/release/SingleEditorStartup.wprp' })) {
        const bound = existingFact(t[key], key), current = fact(path.join(sourceRoot, relative));
        requireThat(samePath(bound.path, current.path) && equal(bound, current), 'trace_tool_identity', key);
        inputs.push(bound);
    }
    if (t.wpr) inputs.push(existingFact(t.wpr, 'wpr'));
    if (t.profile) {
        inputs.push(existingFact(t.profile, 'profile'));
        const escaped = t.process_exe_filter.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
            .replaceAll('"', '&quot;').replaceAll("'", '&apos;');
        requireThat(fs.readFileSync(t.profile.path, 'utf8') === fs.readFileSync(t.profile_template.path, 'utf8')
            .replaceAll('__EDITOR_EXE__', escaped), 'trace_profile_content');
    } else requireThat(t.status === 'failed' && t.started === false, 'trace_profile_missing');
    const allowed = new Set(['profile', 'start', 'stop', 'stop-incomplete', 'cancel-owned']);
    const seen = new Set(); let previous = 0n;
    for (const c of t.commands) {
        requireThat(allowed.has(c.step) && !seen.has(c.step) && Number.isInteger(c.exit_code)
            && Array.isArray(c.arguments) && c.arguments.at(-2) === '-instancename'
            && c.arguments.at(-1) === t.instance, 'trace_command_contract');
        seen.add(c.step);
        const begin = decimal(c.begin_qpc, 'trace_command_qpc', true), end = decimal(c.end_qpc, 'trace_command_qpc', true);
        requireThat(begin >= previous && end >= begin, 'trace_command_order'); previous = end;
        const expected = c.step === 'profile' ? ['-profiles', t.profile?.path]
            : c.step === 'start' ? ['-start', t.profile?.path + '!SingleEditorStartup']
            : c.step === 'cancel-owned' ? ['-cancel'] : ['-stop', t.trace?.path ?? path.join(path.dirname(receiptPath), 'startup.etl'), '-skipPdbGen'];
        requireThat(equal(c.arguments.slice(0, -2), expected), 'trace_command_arguments', c.step);
        requireThat(typeof c.log === 'string' && path.isAbsolute(c.log) && fs.existsSync(c.log), 'trace_command_log');
        inputs.push(fact(c.log));
    }
    requireThat(typeof t.started === 'boolean' && typeof t.stopped === 'boolean', 'trace_collection_state');
    requireThat(t.started === t.commands.some(c => c.step === 'start' && c.exit_code === 0), 'trace_started_state');
    requireThat(t.stopped === t.commands.some(c => ['stop', 'stop-incomplete'].includes(c.step) && c.exit_code === 0),
        'trace_stopped_state');
    let trace = null, core = null;
    if (t.trace) { trace = existingFact(t.trace, 'etl', true); if (trace) inputs.push(trace); }
    if (!trace) pending.push(gap('trace_missing', t.trace?.path ?? null));
    if (t.core_receipt) {
        const f = existingFact(t.core_receipt, 'core_receipt', true);
        if (f) { inputs.push(f); core = read(f.path); } else pending.push(gap('core_receipt_missing', t.core_receipt.path));
    }
    let correlation = null;
    if (t.status === 'captured_pending_analysis') {
        requireThat(t.started && t.stopped && t.error === null && t.candidate && t.profile && t.trace && t.core_receipt && t.wpr,
            'trace_capture_success_contract');
        if (core) correlation = validateTraceCore(core, candidate, t.clock, t.commands);
    } else {
        requireThat(typeof t.error === 'string' && t.error.length > 0, 'trace_capture_failure_reason');
        pending.push(gap('capture_failed', t.error));
        // Incomplete captures remain raw evidence; they cannot supply a successful Core correlation.
    }
    if (!correlation) pending.push(gap('core_correlation_unavailable'));
    return { capture: captureFact, trace, candidate, correlation, pending, inputs, capture_status: t.status,
        profile: t.profile, instance: t.instance, source_root: path.resolve(sourceRoot) };
}

function descriptor(d) {
    return d && ['id', 'version', 'channel', 'level', 'opcode', 'task'].every(k => uint(d[k]))
        && /^0x[A-F0-9]{16}$/.test(d.keyword);
}
// Validate every decoded property against original payload, including unknown event types.
export function validateDecodedEvent(e, sequence) {
    requireThat(e.schema === 'singleeditor.etl-event' && e.version === 1 && e.sequence === sequence
        && uint(e.pid) && uint(e.tid) && guid(e.provider) && descriptor(e.descriptor)
        && typeof e.timestamp_raw === 'string' && /^-?(0|[1-9][0-9]*)$/.test(e.timestamp_raw)
        && uint(e.header_flags) && uint(e.header_properties) && guid(e.activity)
        && uint(e.payload_bytes) && /^[A-F0-9]*$/.test(e.payload_hex) && e.payload_hex.length === e.payload_bytes * 2
        && Array.isArray(e.gaps) && e.tdh && Array.isArray(e.tdh.properties), 'trace_decoded_event');
    requireThat(Array.isArray(e.extended_data) && e.extended_data_count === e.extended_data.length, 'trace_extended_data');
    for (const x of e.extended_data) requireThat(uint(x.type) && uint(x.linkage) && uint(x.bytes)
        && /^[A-F0-9]*$/.test(x.raw_hex) && x.raw_hex.length === x.bytes * 2, 'trace_extended_payload');
    let end = 0;
    for (const p of e.tdh.properties) {
        requireThat(uint(p.index) && uint(p.array_index) && typeof p.name === 'string' && uint(p.flags)
            && uint(p.in_type) && uint(p.out_type) && uint(p.offset) && p.offset === end && uint(p.tdh_status),
            'trace_property_shape');
        if (p.tdh_status !== 0) {
            requireThat(e.gaps.length > 0 && p.bytes === null && p.raw_hex === null && p.formatted === null,
                'trace_failed_property'); break;
        }
        requireThat(uint(p.bytes) && p.offset + p.bytes <= e.payload_bytes && /^[A-F0-9]*$/.test(p.raw_hex)
            && p.raw_hex.length === p.bytes * 2 && p.raw_hex === e.payload_hex.slice(p.offset * 2, (p.offset + p.bytes) * 2)
            && typeof p.formatted === 'string', 'trace_property_payload');
        end += p.bytes;
    }
    if (e.tdh.status === 0) {
        if (e.tdh.provider_guid !== undefined || e.tdh.descriptor !== undefined)
            requireThat(e.tdh.provider_guid === e.provider && equal(e.tdh.descriptor, e.descriptor), 'trace_tdh_descriptor');
        else requireThat(e.gaps.length > 0, 'trace_tdh_metadata_missing');
    }
    if (e.gaps.length === 0) requireThat(e.tdh.status === 0 && end === e.payload_bytes
        && e.tdh.decoded_payload_bytes === e.payload_bytes, 'trace_payload_completeness');
    return e;
}

function typed(e, name, type) {
    const rows = e.tdh.properties.filter(p => p.name === name);
    requireThat(rows.length === 1 && rows[0].in_type === type && rows[0].array_index === 0
        && rows[0].declared_count === 1 && rows[0].tdh_status === 0 && rows[0].flags === 0,
        'trace_semantic_field', name);
    const p = rows[0], b = Buffer.from(p.raw_hex, 'hex');
    if (type === 1) {
        requireThat(b.length >= 2 && b.length % 2 === 0 && b.readUInt16LE(b.length - 2) === 0, 'trace_unicode_field', name);
        const value = b.subarray(0, -2).toString('utf16le');
        requireThat(!value.includes('\0'), 'trace_unicode_embedded_null', name); return value;
    }
    const bytes = type === 8 ? 4 : type === 10 ? 8 : e.tdh.pointer_size;
    requireThat([4, 8].includes(bytes) && b.length === bytes, 'trace_numeric_field', name);
    return bytes === 4 ? BigInt(b.readUInt32LE()) : b.readBigUInt64LE();
}

// Exact registered Windows provider/event versions. Names/formatted strings are not classifiers.
function semantic(e) {
    const d = e.descriptor;
    if (e.gaps.length || e.tdh.status !== 0 || e.tdh.decoding_source !== 0) return null;
    const kw = BigInt(d.keyword) & 0x0000FFFFFFFFFFFFn;
    const processStart = e.provider === PROCESS && d.id === 1 && d.task === 1 && d.opcode === 1
        && d.level === 4 && [0, 1, 2, 3, 4].includes(d.version) && kw === 0x10n;
    const image = e.provider === PROCESS && d.id === 5 && d.task === 5 && d.opcode === 0
        && d.level === 4 && d.version === 0 && kw === 0x40n;
    const read = e.provider === FILE && d.id === 15 && d.task === 15 && d.opcode === 0
        && d.level === 4 && [0, 1].includes(d.version) && kw === 0x120n;
    if (!processStart && !image && !read) return null;
    try {
        if (processStart || image) {
            const processId = typed(e, 'ProcessID', 8), imagePath = typed(e, 'ImageName', 1);
            requireThat(processId <= 0xFFFFFFFFn, 'trace_pid_range');
            return { kind: processStart ? 'process-start' : 'image-load', process_id: Number(processId), image_path: imagePath,
                ...(image ? { image_bytes: typed(e, 'ImageSize', 16).toString(), image_base: typed(e, 'ImageBase', 16).toString() } : {}) };
        }
        return { kind: 'file-read-request', process_id: e.pid, byte_offset: typed(e, 'ByteOffset', 10).toString(),
            requested_bytes: typed(e, 'IOSize', 8).toString(), file_key: typed(e, 'FileKey', 16).toString(),
            file_object: typed(e, 'FileObject', 16).toString(), file_path: null };
    } catch (error) { return { kind: 'unsupported-known-event-shape', detail: error.message }; }
}

export function deriveTraceObservations(header, events, correlation, candidate) {
    const pending = [gap('filtered_system_other_process_io_unobserved'), gap('mapped_page_disk_antivirus_coverage_unproven'),
        gap('optional_trigger_instance_resource_failure_matrix_unobserved'), gap('trace_is_not_performance_evidence')];
    requireThat(header.schema === 'singleeditor.etl-decoder' && header.version === 1
        && ['decoded', 'decoded_with_gaps', 'failed'].includes(header.status) && header.release_eligible === false
        && header.process_trace_mode === RAW_EVENT_RECORD_MODE && Array.isArray(header.gaps)
        && header.event_count === events.length && header.gap_event_count === events.filter(e => e.gaps.length).length,
        'trace_decoder_contract');
    if (header.status === 'failed') pending.push(gap('decoder_failed', header.error));
    else requireThat(header.open_trace_status === 0 && header.process_trace_status === 0 && header.close_trace_status === 0
        && header.error === null, 'trace_decoder_status');
    const session = header.session;
    let qpc = false;
    if (session) {
        requireThat(uint(session.clock_type) && uint(session.events_lost) && uint(session.buffers_lost)
            && uint(session.buffers_written), 'trace_session_fields');
        for (const k of ['perf_frequency', 'start_time', 'end_time', 'boot_time']) decimal(session[k], 'trace_session_decimal');
        if (session.clock_type !== 1 || BigInt(session.perf_frequency) === 0n) pending.push(gap('non_qpc_or_missing_frequency'));
        else if (correlation) {
            requireThat(session.perf_frequency === correlation.frequency, 'trace_decoder_qpc_frequency'); qpc = true;
        }
        if (session.events_lost || session.buffers_lost) pending.push(gap('trace_loss', { events: session.events_lost, buffers: session.buffers_lost }));
        if (session.end_time === '0') pending.push(gap('trace_not_finalized'));
    } else pending.push(gap('trace_session_unavailable'));
    if (header.gaps.length) pending.push(gap('decoder_session_gaps', header.gaps));
    if (header.gap_event_count) pending.push(gap('decoder_event_gaps', header.gap_event_count));
    if (!events.length) pending.push(gap('no_events'));
    const observations = [], unknown = new Map(); let selectedStarts = 0, selectedEvents = 0;
    const summary = { before_core: { image_load_events: 0, file_read_requests: 0, requested_bytes: '0' },
        at_or_after_core: { image_load_events: 0, file_read_requests: 0, requested_bytes: '0' } };
    for (let i = 0; i < events.length; ++i) {
        const e = validateDecodedEvent(events[i], i + 1), value = semantic(e);
        if (!value || value.kind === 'unsupported-known-event-shape') {
            const key = JSON.stringify([e.provider, e.descriptor.id, e.descriptor.version, e.descriptor.opcode,
                value?.detail ?? (e.gaps.length ? 'decoder-event-gaps' : 'unmapped-event')]);
            unknown.set(key, (unknown.get(key) ?? 0) + 1); continue;
        }
        if (!correlation || !qpc || value.process_id !== correlation.process_id) continue;
        const tick = BigInt(e.timestamp_raw), launch = BigInt(correlation.launch), ready = BigInt(correlation.core_ready);
        requireThat(tick >= launch && tick <= BigInt(correlation.trace_stop_end), 'trace_selected_pid_qpc_interval');
        ++selectedEvents;
        if (value.kind === 'process-start') {
            ++selectedStarts;
            requireThat(tick <= ready, 'trace_process_start_after_core');
            if (/^[A-Za-z]:[\\/]/.test(value.image_path)) requireThat(samePath(value.image_path, candidate.path), 'trace_process_image_path');
            else pending.push(gap('process_image_path_unmapped', value.image_path));
        }
        const phase = tick < ready ? 'before_core' : 'at_or_after_core';
        observations.push({ sequence: e.sequence, provider: e.provider, descriptor: e.descriptor, timestamp_qpc: e.timestamp_raw,
            header_process_id: e.pid, phase, ...value });
        if (value.kind === 'image-load') ++summary[phase].image_load_events;
        if (value.kind === 'file-read-request') {
            ++summary[phase].file_read_requests;
            summary[phase].requested_bytes = (BigInt(summary[phase].requested_bytes) + BigInt(value.requested_bytes)).toString();
        }
    }
    requireThat(selectedStarts <= 1, 'trace_pid_reused_or_duplicate_start');
    if (!selectedStarts) pending.push(gap('target_process_start_unobserved'));
    if (!selectedEvents) pending.push(gap('target_pid_events_unobserved'));
    if (unknown.size) pending.push(gap('unmapped_events', [...unknown].map(([key, count]) => ({ event: JSON.parse(key), count }))));
    if (observations.some(x => x.kind === 'file-read-request')) pending.push(gap('read_filekey_path_correlation_unproven'));
    return { status: 'pending', release_eligible: false, performance_eligible: false, correlation,
        summary, observations, pending, scope: 'Observed selected-PID image loads and file read requests only. Request sizes are not completed reads or physical disk bytes. No absence, lazy activation, instance, resource or failure proof.' };
}

function decode(input, decoder, output) {
    requireThat(!fs.existsSync(output), 'trace_decode_output_exists');
    const before = existingFact(input, 'etl_before_decode'), tool = fact(decoder);
    const run = spawnSync(tool.path, [before.path, output], { windowsHide: true, encoding: 'utf8', timeout: 120000, maxBuffer: 1048576 });
    requireThat(!run.error && [0, 1, 3].includes(run.status), 'trace_decoder_execution', run.error?.message ?? run.stderr ?? String(run.status));
    const headerPath = path.join(output, 'header.json');
    requireThat(fs.existsSync(headerPath), 'trace_decoder_no_receipt');
    const h = read(headerPath), inputs = [before, tool];
    for (const [f, label] of [[h.input, 'decoder_input'], [h.input_after, 'decoder_input_after'], [h.decoder, 'decoder_executable']])
        inputs.push(existingFact(f, label));
    requireThat(equal(h.input, before) && equal(h.input_after, before) && h.input_unchanged === true
        && equal(h.decoder, tool), 'trace_decoder_binding');
    const eventFact = existingFact(h.events, 'decoded_events');
    requireThat(samePath(eventFact.path, path.join(output, 'events.jsonl')), 'trace_events_location');
    const data = fs.readFileSync(eventFact.path, 'utf8');
    requireThat(!data.length || data.endsWith('\n'), 'trace_events_truncated');
    const events = data.length ? data.slice(0, -1).split('\n').map((x, i) => validateDecodedEvent(JSON.parse(x), i + 1)) : [];
    requireThat((run.status === 1) === (h.status === 'failed') && (run.status === 3) === (h.status === 'decoded_with_gaps'),
        'trace_decoder_exit_status');
    afterFacts(inputs);
    return { header: h, events, header_fact: fact(headerPath), events_fact: eventFact,
        execution: { exit_code: run.status, stdout: run.stdout, stderr: run.stderr } };
}

const emptyDerived = pending => ({ status: 'pending', release_eligible: false, performance_eligible: false,
    correlation: null, summary: null, observations: [], pending,
    scope: 'No analyzable ETL/Core pair; no event, timing, absence or lifecycle conclusion.' });
function derive(capture, decoded) {
    if (capture.capture_status === 'captured_pending_analysis' && decoded)
        requireThat(decoded.header.status !== 'failed', 'trace_successful_capture_decode_failed', decoded.header.error);
    const result = decoded ? deriveTraceObservations(decoded.header, decoded.events, capture.correlation, capture.candidate)
        : emptyDerived([]);
    result.pending = [...capture.pending, ...result.pending];
    return result;
}
export function analyzeStartupTrace(receiptPath, decoderBuildPath, output, sourceRoot = path.resolve(path.dirname(self), '../..')) {
    const capture = captureInputs(path.resolve(receiptPath), sourceRoot);
    requireThat(!fs.existsSync(output), 'trace_analysis_output_exists');
    const build = decoderBuild(decoderBuildPath), decoderFact = build.decoder;
    fs.mkdirSync(output, { recursive: false });
    const decoded = capture.trace ? decode(capture.trace, decoderFact.path, path.join(output, 'decoded')) : null;
    const derived = derive(capture, decoded);
    afterFacts([...capture.inputs, ...build.files, ...build.current_inputs]);
    const result = { schema: 'singleeditor.startup-trace-analysis', version: 1, status: 'pending', release_eligible: false,
        performance_eligible: false, source_root: capture.source_root, capture: capture.capture,
        candidate: capture.candidate, analyzer: fact(self), decoder: decoderFact, decoder_build: build, inputs: capture.inputs,
        decoded: decoded ? { header: decoded.header_fact, events: decoded.events_fact, execution: decoded.execution } : null,
        derived };
    write(path.join(output, 'receipt.json'), result); return result;
}

export function verifyStartupTraceAnalysis(receiptPath, selectedGui) {
    const before = fact(receiptPath), receipt = read(receiptPath);
    requireThat(receipt.schema === 'singleeditor.startup-trace-analysis' && receipt.version === 1
        && receipt.status === 'pending' && receipt.release_eligible === false && receipt.performance_eligible === false,
        'trace_analysis_contract');
    requireThat(equal(existingFact(receipt.analyzer, 'analyzer'), fact(self)), 'trace_analyzer_identity');
    const decoderFact = existingFact(receipt.decoder, 'decoder');
    const build = decoderBuild(receipt.decoder_build?.receipt?.path);
    requireThat(equal(build, receipt.decoder_build) && equal(build.decoder, decoderFact), 'trace_decoder_build_changed');
    existingFact(receipt.capture, 'capture');
    const capture = captureInputs(receipt.capture.path, receipt.source_root, path.resolve(selectedGui));
    requireThat(equal(capture.candidate, receipt.candidate) && equal(capture.inputs, receipt.inputs), 'trace_analysis_input_inventory');
    let decoded = null, replay = null;
    if (capture.trace) {
        requireThat(receipt.decoded, 'trace_analysis_decoded_missing');
        existingFact(receipt.decoded.header, 'saved_decoder_header'); existingFact(receipt.decoded.events, 'saved_decoder_events');
        requireThat(samePath(receipt.decoded.header.path, path.join(path.dirname(receiptPath), 'decoded/header.json'))
            && samePath(receipt.decoded.events.path, path.join(path.dirname(receiptPath), 'decoded/events.jsonl')), 'trace_saved_decode_location');
        const saved = read(receipt.decoded.header.path);
        existingFact(saved.input, 'saved_decoder_input'); existingFact(saved.input_after, 'saved_decoder_input_after');
        existingFact(saved.decoder, 'saved_decoder_executable'); existingFact(saved.events, 'saved_decoder_event_fact');
        requireThat(equal(saved.input, capture.trace) && equal(saved.input_after, capture.trace) && equal(saved.decoder, decoderFact)
            && equal(saved.events, receipt.decoded.events), 'trace_saved_decode_binding');
        const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'singleeditor-etl-verify-'));
        decoded = decode(capture.trace, decoderFact.path, path.join(scratch, 'decoded'));
        // Exactly one path varies. Both event facts have independently verified absolute location, size and SHA.
        const comparable = h => ({ ...h, events: { ...h.events, path: '<verified-current-decode-events>' } });
        requireThat(equal(comparable(saved), comparable(decoded.header))
            && receipt.decoded.events.bytes === decoded.events_fact.bytes
            && receipt.decoded.events.sha256 === decoded.events_fact.sha256, 'trace_offline_redecode_mismatch');
        requireThat(equal(receipt.decoded.execution, decoded.execution), 'trace_decode_execution_mismatch');
        replay = { directory: scratch, header: decoded.header_fact, events: decoded.events_fact,
            excluded_header_fields: ['events.path'], exclusion_validation: 'Both exact decoded/events.jsonl paths, existence, bytes and SHA independently verified.' };
    } else requireThat(receipt.decoded === null, 'trace_unavailable_has_decode');
    const derived = derive(capture, decoded);
    requireThat(equal(receipt.derived, derived), 'trace_derived_observation_mismatch');
    afterFacts([...capture.inputs, ...build.files, ...build.current_inputs]); existingFact(before, 'analysis_after_verify');
    return { schema: 'singleeditor.startup-trace-analysis-verification', version: 1, status: 'pending', release_eligible: false,
        performance_eligible: false, verifier: fact(self), analysis: before, candidate: capture.candidate,
        capture: capture.capture, replay, derived, scope: 'Fresh offline verification only; complete startup/lazy-trigger release gate remains pending.' };
}

if (process.argv[1] && samePath(process.argv[1], self)) {
    try {
        const [mode, ...args] = process.argv.slice(2);
        let result;
        if (mode === 'analyze' && [3, 5].includes(args.length)) {
            requireThat(args.length === 3 || args[3] === '--source-root', 'trace_cli_arguments');
            result = analyzeStartupTrace(args[0], args[1], path.resolve(args[2]), args[4]);
        } else if (mode === 'verify' && args.length === 2) result = verifyStartupTraceAnalysis(path.resolve(args[0]), args[1]);
        else throw new Error('Usage: StartupTraceAnalysis.mjs analyze capture.json decoder-build-receipt.json new-output [--source-root root] | verify analysis.json selected-gui.exe');
        console.log(JSON.stringify(result)); process.exitCode = 3;
    } catch (error) { console.error(error.message); process.exitCode = 1; }
}
