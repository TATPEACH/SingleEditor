// Build/release tooling only. Observe the selected MSVC build; never load a
// product image or derive semantic ownership from its directory or symbol name.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { pathToFileURL } from 'node:url';

const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex').toUpperCase();
const readJson = p => JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
const key = p => path.resolve(p).replaceAll('\\', '/').toLowerCase();
function requireFact(ok, code, detail) { if (!ok) throw new Error(`${code}: ${detail}`); }
function fileFact(p) { const b = fs.readFileSync(p); return { bytes: b.length, sha256: sha(b) }; }
function lines(p) {
    const b = fs.readFileSync(p);
    return b.toString(b[0] === 255 && b[1] === 254 ? 'utf16le' : 'utf8')
        .replace(/^\uFEFF/, '').split(/\r?\n/).filter(Boolean);
}
function trackedRecords(p) {
    const records = []; let current;
    for (const line of lines(p)) {
        if (line.startsWith('^')) { current = { inputs: line.slice(1).split('|'), values: [] }; records.push(current); }
        else { requireFact(current, 'unsupported_tracking_log', p); current.values.push(line); }
    }
    return records;
}

export function reconcileSources(expected, observed) {
    const declared = new Set(expected.map(key)), seen = new Set();
    requireFact(declared.size === expected.length, 'duplicate_declared_source', 'CMake compilation graph');
    for (const source of observed) {
        requireFact(!seen.has(key(source)), 'duplicate_observed_source', source); seen.add(key(source));
        requireFact(declared.has(key(source)), 'observed_source_undeclared', source);
    }
    for (const source of expected) requireFact(seen.has(key(source)), 'declared_source_missing', source);
}

// This is a compile-boundary check for the reviewed test-only observers, not a
// name-based claim that the entire D159 forbidden set has been proved absent.
export function validateTestObserverIsolation(target, groups, commands) {
    const macro = 'SINGLE_EDITOR_ENABLE_TEST_OBSERVERS';
    const product = ['SingleEditor', 'SingleEditorCLI'].includes(target);
    const check = (definitions, where) => {
        const values = definitions.filter(d => d === macro || d.startsWith(macro + '='));
        requireFact(values.every(d => d === macro + '=0') && (!product || values.length > 0),
            'test_observers_not_isolated', `${target} ${where}`);
    };
    for (const [index, group] of groups.entries())
        check((group.defines ?? []).map(d => d.define), `configured group ${index}`);
    for (const [index, command] of commands.entries()) {
        const text = command.values.join(' ');
        const definitions = [...text.matchAll(/(?:^|\s)[/-]D\s*(?:"([^"]*)"|([^\s"]+))/gi)]
            .map(m => m[1] ?? m[2]);
        requireFact(!new RegExp(`(?:^|\\s)[/-]U\\s*"?${macro}(?:"|\\s|$)`, 'i').test(text),
            'test_observers_not_isolated', `${target} command ${index} undefines boundary`);
        check(definitions, `observed command ${index}`);
    }
    requireFact(!product || (groups.length > 0 && commands.length > 0),
        'test_observers_not_isolated', `${target} missing compilation evidence`);
    return { macro, value: 0, configured_groups: groups.length, observed_commands: commands.length,
        scope: 'Reviewed internal observers disabled in actual compilation; binary/runtime proof remains separate.' };
}

// Read the standard PE directories of our own linked artifacts. The byte ranges
// and directory IDs follow Microsoft's PE/COFF specification. A fact that this
// scanner cannot interpret is an error or an explicit pending item, never zero.
export function peFacts(filename) {
    const b = fs.readFileSync(filename);
    const range = (p, n) => {
        requireFact(Number.isSafeInteger(p) && p >= 0 && n >= 0 && p + n <= b.length,
            'unsupported_pe_range', `${filename} @ ${p}+${n}`); return b.subarray(p, p + n);
    };
    const u16 = p => range(p, 2).readUInt16LE(), u32 = p => range(p, 4).readUInt32LE();
    requireFact(range(0, 2).toString() === 'MZ', 'unsupported_pe', filename);
    const pe = u32(0x3c), o = pe + 24;
    requireFact(range(pe, 4).toString() === 'PE\0\0' && u16(o) === 0x20b,
        'unsupported_pe', 'This product scanner requires PE32+.');
    const headersBytes = u32(o + 60), optionalBytes = u16(pe + 20), directoryCount = u32(o + 108);
    const sectionTable = o + optionalBytes, sectionTableEnd = sectionTable + u16(pe + 6) * 40;
    requireFact(u16(pe + 6) > 0 && u16(pe + 6) <= 96 && directoryCount <= 16
        && optionalBytes >= 112 + directoryCount * 8 && sectionTableEnd <= headersBytes,
        'unsupported_pe_header_layout', filename);
    range(0, headersBytes);
    const sections = [];
    for (let i = 0, p = o + u16(pe + 20); i < u16(pe + 6); ++i, p += 40) {
        const offset = u32(p + 20), bytes = u32(p + 16);
        sections.push({ name: range(p, 8).toString('ascii').replace(/\0.*$/, ''),
            virtual_bytes: u32(p + 8), rva: u32(p + 12), raw_bytes: bytes, file_offset: offset,
            characteristics: u32(p + 36), raw_sha256: sha(range(offset, bytes)) });
    }
    const at = (rva, count = 1) => {
        const s = sections.find(s => rva >= s.rva && rva + count <= s.rva + s.raw_bytes);
        requireFact(s, 'unsupported_pe_rva', `${filename} @ ${rva}+${count}`);
        const p = s.file_offset + rva - s.rva; range(p, count); return p;
    };
    const stringAt = rva => {
        const p = at(rva), s = sections.find(s => p >= s.file_offset && p < s.file_offset + s.raw_bytes);
        const end = b.indexOf(0, p);
        requireFact(end >= p && end < s.file_offset + s.raw_bytes, 'unsupported_pe_string', filename);
        return range(p, end - p).toString('ascii');
    };
    const dir = index => {
        requireFact(u32(o + 108) > index, 'unsupported_pe_directory', String(index));
        return { address: u32(o + 112 + index * 8), bytes: u32(o + 116 + index * 8) };
    };
    const imports = [], delayImports = [];
    const importDir = dir(1);
    if (importDir.bytes) {
        const begin = at(importDir.address, importDir.bytes); let terminated = false;
        for (let p = begin; p + 20 <= begin + importDir.bytes; p += 20) {
            if (range(p, 20).every(x => x === 0)) { terminated = true; break; }
            const names = [], tableRva = u32(p) || u32(p + 16);
            for (let i = 0; ; ++i) {
                const item = range(at(tableRva + i * 8, 8), 8).readBigUInt64LE();
                if (item === 0n) break;
                names.push(item & 0x8000000000000000n
                    ? { ordinal: Number(item & 0xffffn) }
                    : { name: stringAt(Number(item) + 2), hint: u16(at(Number(item), 2)) });
            }
            imports.push({ dll: stringAt(u32(p + 12)).toLowerCase(), symbols: names });
        }
        requireFact(terminated, 'unsupported_pe_imports', filename);
    }
    // Keep delay-import bytes visible until the dedicated decoder is present.
    const delayed = dir(13);
    if (delayed.bytes) delayImports.push({ status: 'unsupported', directory: delayed,
        sha256: sha(range(at(delayed.address, delayed.bytes), delayed.bytes)) });
    const exports = dir(0);
    const resources = [], resourceDir = dir(2), metadata = new Map();
    const addMetadata = (fileOffset, count, kind, descendants, reference, isRoot = false) => {
        const id = `${fileOffset}:${count}`;
        if (!metadata.has(id)) metadata.set(id, { kind, file_offset: fileOffset, bytes: count,
            is_root: isRoot, descendant_keys: new Set(), references: new Map() });
        const span = metadata.get(id);
        requireFact(span.kind === kind && span.is_root === isRoot,
            'unsupported_resource_metadata_alias', id);
        descendants.forEach(k => span.descendant_keys.add(k));
        span.references.set(JSON.stringify(reference), reference);
    };
    if (resourceDir.bytes) {
        const base = at(resourceDir.address, resourceDir.bytes), active = new Set();
        const resourceRange = (offset, bytes) => {
            requireFact(offset >= 0 && offset + bytes <= resourceDir.bytes, 'unsupported_resource_range', filename);
            return base + offset;
        };
        const nameOf = n => {
            if (!(n & 0x80000000)) return n;
            const p = resourceRange(n & 0x7fffffff, 2), len = u16(p);
            return range(resourceRange((n & 0x7fffffff) + 2, len * 2), len * 2).toString('utf16le');
        };
        const walk = (offset, ids, referrer = null) => {
            requireFact(ids.length <= 8 && !active.has(offset), 'unsupported_resource_tree', filename);
            active.add(offset);
            const p = resourceRange(offset, 16), count = u16(p + 12) + u16(p + 14);
            resourceRange(offset + 16, count * 8);
            const descendants = new Set();
            for (let i = 0; i < count; ++i) {
                const entry = p + 16 + i * 8, name = u32(entry), id = nameOf(name), child = u32(entry + 4);
                const route = [...ids, id], reference = { ids: route, referrer_file_offset: entry };
                let leaves;
                if (child & 0x80000000) leaves = walk(child & 0x7fffffff, route, entry);
                else {
                    const data = resourceRange(child, 16), rva = u32(data), bytes = u32(data + 4);
                    const fileOffset = at(rva, bytes);
                    resources.push({ ids: route, rva, file_offset: fileOffset, bytes, code_page: u32(data + 8),
                        sha256: sha(range(fileOffset, bytes)) });
                    leaves = new Set([JSON.stringify(route)]);
                    addMetadata(data, 16, 'resource-data-entry', leaves, reference);
                }
                addMetadata(entry, 8, 'resource-directory-entry', leaves, reference);
                if (name & 0x80000000) {
                    const nameOffset = resourceRange(name & 0x7fffffff, 2), count = 2 + u16(nameOffset) * 2;
                    resourceRange(name & 0x7fffffff, count);
                    addMetadata(nameOffset, count, 'resource-name', leaves, reference);
                }
                leaves.forEach(k => descendants.add(k));
            }
            addMetadata(p, 16, 'resource-directory-header', descendants,
                { ids, referrer_file_offset: referrer }, offset === 0);
            active.delete(offset);
            return descendants;
        };
        walk(0, []);
    }
    const certificates = dir(4); // Unlike other directories, this is a file offset.
    const debug = dir(6), debugTypes = [];
    if (debug.bytes) {
        requireFact(debug.bytes % 28 === 0, 'unsupported_pe_debug', filename);
        const p = at(debug.address, debug.bytes);
        for (let i = 0; i < debug.bytes; i += 28) debugTypes.push(u32(p + i + 12));
    }
    const alignUp = (n, alignment) => Math.ceil(n / alignment) * alignment;
    const fileAlignment = u32(o + 36), sectionAlignment = u32(o + 32);
    requireFact(fileAlignment >= 512 && fileAlignment <= 65536 && (fileAlignment & (fileAlignment - 1)) === 0
        && sectionAlignment >= fileAlignment && (sectionAlignment & (sectionAlignment - 1)) === 0,
        'unsupported_pe_alignment', filename);
    const directories = Array.from({ length: directoryCount }, (_, index) => {
        const d = dir(index);
        return { index, address: d.address, ...(index === 4 ? {} : { rva: d.address }), bytes: d.bytes,
            file_offset: d.bytes ? index === 4 ? d.address : at(d.address, d.bytes) : null };
    });
    const metadataSpans = [...metadata.values()].map(s => ({ ...s,
        descendant_keys: [...s.descendant_keys].sort(), references: [...s.references.values()]
            .sort((a, b) => JSON.stringify(a).localeCompare(JSON.stringify(b), 'en')) }))
        .sort((a, b) => a.file_offset - b.file_offset || a.bytes - b.bytes);
    const contentSpans = resources.map(r => ({ file_offset: r.file_offset, bytes: r.bytes }));
    let metadataEnd = -1;
    for (const s of metadataSpans) {
        requireFact(s.file_offset >= metadataEnd, 'unsupported_resource_metadata_overlap', filename);
        requireFact(!contentSpans.some(r => s.file_offset < r.file_offset + r.bytes
            && r.file_offset < s.file_offset + s.bytes), 'unsupported_resource_metadata_content_overlap', filename);
        metadataEnd = s.file_offset + s.bytes;
    }
    const packingGaps = [];
    if (resourceDir.bytes) {
        const begin = at(resourceDir.address, resourceDir.bytes), end = begin + resourceDir.bytes;
        let cursor = begin;
        for (const s of [...metadataSpans, ...contentSpans].sort((a, b) => a.file_offset - b.file_offset)) {
            const start = Math.max(begin, s.file_offset), stop = Math.min(end, s.file_offset + s.bytes);
            if (start >= stop) continue;
            if (start > cursor) packingGaps.push({ file_offset: cursor, bytes: start - cursor,
                all_zero: range(cursor, start - cursor).every(v => v === 0) });
            cursor = Math.max(cursor, stop);
        }
        if (cursor < end) packingGaps.push({ file_offset: cursor, bytes: end - cursor,
            all_zero: range(cursor, end - cursor).every(v => v === 0) });
    }
    const relocDir = dir(5), relocations = { status: relocDir.bytes ? 'parsed' : 'absent',
        file_offset: relocDir.bytes ? at(relocDir.address, relocDir.bytes) : null,
        rva: relocDir.address, bytes: relocDir.bytes, blocks: [], unsupported: [] };
    if (relocDir.bytes) {
        const begin = relocations.file_offset, end = begin + relocDir.bytes, fixups = [];
        for (let p = begin; p < end;) {
            requireFact(p + 8 <= end, 'unsupported_relocation_block', `${p}: truncated header`);
            const page = u32(p), count = u32(p + 4);
            requireFact(count >= 8 && count % 4 === 0 && p + count <= end && page % 4096 === 0,
                'unsupported_relocation_block', `${p}: size/page`);
            const block = { file_offset: p, bytes: count, page_rva: page, entries: [] };
            for (let q = p + 8; q < p + count; q += 2) {
                const raw = u16(q), type = raw >>> 12, target = page + (raw & 4095);
                const entry = { file_offset: q, bytes: 2, type, target_rva: type === 0 ? null : target,
                    target_file_offset: null, target_bytes: type === 10 ? 8 : 0 };
                if (type === 10) {
                    const section = sections.find(s => target >= s.rva
                        && target + 8 <= s.rva + Math.min(s.raw_bytes, s.virtual_bytes));
                    requireFact(section, 'unsupported_relocation_target', `${q}: ${target}+8`);
                    entry.target_file_offset = section.file_offset + target - section.rva;
                    fixups.push({ rva: target, bytes: 8 });
                } else if (type !== 0) {
                    relocations.status = 'unsupported';
                    relocations.unsupported.push({ code: 'unsupported_relocation_type', file_offset: q, type });
                }
                block.entries.push(entry);
            }
            relocations.blocks.push(block); p += count;
        }
        fixups.sort((a, b) => a.rva - b.rva);
        for (let i = 1; i < fixups.length; ++i) requireFact(fixups[i - 1].rva + 8 <= fixups[i].rva,
            'unsupported_relocation_target_overlap', `${fixups[i].rva}`);
    }
    const rawPadding = [], imagePadding = [];
    let previousImageEnd = headersBytes, previousFileEnd = headersBytes;
    for (const s of sections) {
        requireFact(s.rva >= previousImageEnd && s.file_offset >= previousFileEnd,
            'unsupported_pe_section_overlap', s.name);
        if (s.rva > previousImageEnd) imagePadding.push({ rva: previousImageEnd, bytes: s.rva - previousImageEnd,
            eligible: s.rva === alignUp(previousImageEnd, sectionAlignment),
            reason: s.rva === alignUp(previousImageEnd, sectionAlignment) ? 'exact-section-alignment' : 'excess-image-gap' });
        previousImageEnd = s.rva + s.virtual_bytes; previousFileEnd = s.file_offset + s.raw_bytes;
        if (s.raw_bytes > s.virtual_bytes) {
            const offset = s.file_offset + s.virtual_bytes, count = s.raw_bytes - s.virtual_bytes;
            const exact = s.raw_bytes === alignUp(s.virtual_bytes, fileAlignment), zero = range(offset, count).every(v => v === 0);
            const clear = !directories.some(d => d.bytes && d.file_offset < offset + count && offset < d.file_offset + d.bytes);
            rawPadding.push({ section: s.name, file_offset: offset, bytes: count, eligible: exact && zero && clear,
                reason: !exact ? 'excess-file-suffix' : !zero ? 'nonzero-file-suffix' : !clear ? 'directory-overlap' : 'exact-zero-file-alignment' });
        }
    }
    const imageBytes = u32(o + 56);
    requireFact(previousImageEnd <= imageBytes, 'unsupported_pe_image_extent', filename);
    if (imageBytes > previousImageEnd) imagePadding.push({ rva: previousImageEnd, bytes: imageBytes - previousImageEnd,
        eligible: imageBytes === alignUp(previousImageEnd, sectionAlignment),
        reason: imageBytes === alignUp(previousImageEnd, sectionAlignment) ? 'exact-image-alignment' : 'excess-image-tail' });
    const peLayout = { schema: 'singleeditor.pe-layout-facts', version: 1,
        file_alignment_bytes: fileAlignment, section_alignment_bytes: sectionAlignment, directories,
        headers: { file_offset: 0, rva: 0, bytes: headersBytes, status: 'parsed',
            producer_proof: { status: 'unsupported', code: 'header_link_provenance_required' },
            components: [{ kind: 'dos-link-envelope', file_offset: 0, bytes: pe },
                { kind: 'pe-signature-and-coff', file_offset: pe, bytes: 24 },
                { kind: 'pe-optional-header', file_offset: o, bytes: optionalBytes },
                { kind: 'pe-section-table', file_offset: sectionTable, bytes: sectionTableEnd - sectionTable },
                { kind: 'pe-header-alignment', file_offset: sectionTableEnd, bytes: headersBytes - sectionTableEnd,
                    all_zero: range(sectionTableEnd, headersBytes - sectionTableEnd).every(v => v === 0) }] },
        relocations, file_alignment: { ranges: rawPadding }, image_alignment: { ranges: imagePadding },
        resource_metadata: { status: resourceDir.bytes ? 'parsed' : 'absent',
            root_file_offset: resourceDir.bytes ? at(resourceDir.address, resourceDir.bytes) : null,
            spans: metadataSpans, internal_packing_gaps: packingGaps } };
    return { ...fileFact(filename), machine: u16(pe + 4), image_bytes: u32(o + 56),
        headers_bytes: u32(o + 60), sections, imports, delay_imports: delayImports,
        exports: exports.bytes ? { status: 'unsupported', directory: exports } : { status: 'absent', count: 0 },
        resources, debug_types: debugTypes, pe_layout: peLayout,
        embedded_certificate: certificates.bytes ? { status: 'present-unverified', bytes: certificates.bytes,
            sha256: sha(range(certificates.address, certificates.bytes)) } : { status: 'absent', bytes: 0 },
        signature_validation: 'not-performed; catalog signature and trust are not inferred from embedded certificates' };
}

// A parsed header is not sufficient producer evidence. This proof is limited to
// the recorded MSVC default-DOS, same-link finalization route used by this build.
export function peHeaderProducerProof(filename, pe, capturePath, product, roots) {
    const inputs = [], checked = new Map();
    const verify = f => {
        requireFact(f && typeof f.path === 'string' && /^[0-9a-f]{64}$/i.test(f.sha256),
            'header_provenance_fact_missing', String(f?.path));
        const p = path.resolve(f.path), bytes = fs.readFileSync(p), actual = sha(bytes);
        requireFact(actual === f.sha256.toUpperCase() && (f.bytes === undefined || bytes.length === f.bytes),
            'header_provenance_input_changed', p);
        if (!checked.has(key(p))) { const item = { path: p, bytes: bytes.length, sha256: actual };
            checked.set(key(p), item); inputs.push(item); }
        return bytes;
    };
    const resolve = p => p.replace('<BUILD>', roots.build).replace('<SOURCE>', roots.source)
        .replace('<PAYLOAD>', roots.payload);
    try {
        const image = fs.readFileSync(filename), imageHash = sha(image), captureBytes = fs.readFileSync(capturePath);
        const capture = JSON.parse(captureBytes.toString('utf8').replace(/^\uFEFF/, ''));
        verify({ path: capturePath, bytes: captureBytes.length, sha256: sha(captureBytes) });
        requireFact(capture.schema === 'singleeditor.clean-build-capture' && capture.version === 1 && capture.phase === 'built'
            && key(capture.build_root) === key(roots.build) && key(capture.source_root) === key(roots.source),
            'header_provenance_capture_mismatch', product.target);
        const built = capture.binaries.filter(p => p.target === product.target);
        requireFact(built.length === 1 && built[0].sha256 === imageHash && product.sha256 === imageHash
            && pe.sha256 === imageHash, 'header_provenance_product_mismatch', product.target);
        requireFact(/set\(CMAKE_CXX_COMPILER_ID "MSVC"\)/.test(capture.inputs.compiler)
            && /set\(CMAKE_CXX_COMPILER_LINKER_ID "MSVC"\)/.test(capture.inputs.compiler),
            'header_provenance_linker_unsupported', product.target);
        const linkerPath = /set\(CMAKE_LINKER "([^"]+)"\)/.exec(capture.inputs.compiler)?.[1];
        const linkers = capture.inputs.toolchain.filter(f => linkerPath && key(f.path) === key(linkerPath));
        requireFact(linkers.length === 1 && path.basename(linkerPath).toLowerCase() === 'link.exe',
            'header_provenance_linker_missing', product.target);
        verify(linkers[0]);
        const commandPath = resolve(product.link_command.path);
        requireFact(key(commandPath) === key(path.join(roots.build, `${product.target}.dir`, 'Release',
            `${product.target}.tlog`, 'link.command.1.tlog')), 'header_provenance_link_command_target', product.target);
        verify({ ...product.link_command, path: commandPath });
        const records = trackedRecords(commandPath);
        requireFact(records.length === 1, 'header_provenance_link_records', product.target);
        const command = records[0].values.join(' ');
        requireFact(!/(?:^|\s)"?[/-](?:STUB|DLL|DRIVER)(?::|"|\s|$)/i.test(command)
            && !/(?:^|\s)"?@/.test(command), 'header_provenance_custom_or_indirect_link', product.target);
        const outs = [...command.matchAll(/(?:^|\s)[/-]OUT:(?:"([^"]+)"|(\S+))/gi)];
        const machines = [...command.matchAll(/(?:^|\s)[/-]MACHINE:(\S+)/gi)];
        requireFact(outs.length === 1 && key(outs[0][1] ?? outs[0][2]) === key(built[0].path)
            && machines.length > 0 && machines.every(m => m[1].toUpperCase() === 'X64'),
            'header_provenance_link_flags', product.target);
        const envelopeEnd = image.readUInt32LE(60), optional = envelopeEnd + 24;
        const stockHeader = Buffer.from('4d5a90000300000004000000ffff0000b800000000000000400000000000000000000000000000000000000000000000000000000000000000000000', 'hex');
        const stockStub = Buffer.from('0e1fba0e00b409cd21b8014ccd21546869732070726f6772616d2063616e6e6f742062652072756e20696e20444f53206d6f64652e0d0d0a2400000000000000', 'hex');
        requireFact(envelopeEnd >= 128 && image.subarray(0, 60).equals(stockHeader)
            && image.subarray(64, 128).equals(stockStub), 'header_provenance_nonstock_dos', product.target);
        // The extra producer metadata must have the MSVC DanS/records/Rich form,
        // not arbitrary bytes accepted because they preceded the PE signature.
        const rich = image.indexOf(Buffer.from('Rich'), 128);
        requireFact(rich >= 144 && rich + 8 <= envelopeEnd && (rich - 144) % 8 === 0,
            'header_provenance_rich_layout', product.target);
        const xor = image.readUInt32LE(rich + 4), word = p => (image.readUInt32LE(p) ^ xor) >>> 0;
        requireFact(word(128) === 0x536e6144 && word(132) === 0 && word(136) === 0 && word(140) === 0
            && image.subarray(rich + 8, envelopeEnd).every(v => v === 0),
            'header_provenance_rich_layout', product.target);
        const headerTail = pe.pe_layout.headers.components.find(c => c.kind === 'pe-header-alignment');
        requireFact(headerTail?.all_zero && pe.headers_bytes === Math.ceil(headerTail.file_offset
            / pe.pe_layout.file_alignment_bytes) * pe.pe_layout.file_alignment_bytes,
            'header_provenance_header_alignment', product.target);
        const link = product.link_finalization;
        requireFact(link?.status === 'same-link-finalized' && link.final_image_sha256 === imageHash,
            'header_provenance_finalization_required', product.target);
        const recordBytes = verify({ ...link.record, path: resolve(link.record.path) });
        const record = JSON.parse(recordBytes.toString('utf8').replace(/^\uFEFF/, ''));
        requireFact(record.schema === 'singleeditor.product-link-finalization' && record.version === 1
            && record.status === 'build-finalized' && record.target === product.target
            && record.final_image.sha256 === imageHash && key(record.final_image.path) === key(built[0].path),
            'header_provenance_finalization_target', product.target);
        const original = verify(record.linked_image);
        const finalizationPath = resolve(link.finalization.path);
        requireFact(key(record.finalization.path) === key(finalizationPath)
            && record.finalization.sha256 === link.finalization.sha256,
            'header_provenance_finalization_binding', product.target);
        const finalized = JSON.parse(verify({ ...link.finalization, path: finalizationPath }).toString('utf8'));
        requireFact(finalized.schema === 'singleeditor.link-evidence-finalization' && finalized.version === 1
            && finalized.status === 'probe-verified' && finalized.unchanged_runtime_layout === true
            && finalized.inputs.image.sha256 === record.linked_image.sha256 && finalized.output.sha256 === imageHash,
            'header_provenance_finalization_chain', product.target);
        verify(finalized.output);
        requireFact(record.archived_tools?.length > 0, 'header_provenance_archived_tools_missing', product.target);
        for (const f of record.archived_tools) verify(f);
        requireFact(record.archived_tools.some(f => f.sha256 === finalized.inputs.finalizer.sha256),
            'header_provenance_finalizer_not_archived', product.target);
        const debugSizeOffset = optional + 116 + 6 * 8;
        const headerEdits = finalized.edits.filter(e => e.offset < pe.headers_bytes);
        requireFact(headerEdits.length === 1 && headerEdits[0].offset === debugSizeOffset
            && headerEdits[0].bytes === 4 && headerEdits[0].reason === 'debug-directory-size'
            && original.length === image.length && original.subarray(0, debugSizeOffset).equals(image.subarray(0, debugSizeOffset))
            && original.subarray(debugSizeOffset + 4, pe.headers_bytes).equals(image.subarray(debugSizeOffset + 4, pe.headers_bytes)),
            'header_provenance_unexplained_header_edit', product.target);
        return { status: 'proven', code: 'captured-msvc-default-envelope-same-link-v1', target: product.target,
            source_image_sha256: imageHash, inputs, dos_envelope_bytes: envelopeEnd,
            header_sha256: sha(image.subarray(0, pe.headers_bytes)),
            permitted_finalization_header_edit: headerEdits[0],
            scope: 'Recorded MSVC linker and direct flags, stock DOS code and producer-metadata form, exact linked header preserved except declared debug-directory size; no owner inference.' };
    } catch (error) {
        return { status: 'unsupported', code: String(error.message).split(':')[0],
            detail: String(error.message), target: product?.target ?? null, source_image_sha256: pe.sha256, inputs };
    }
}

export function observeBuild(capturePath, payloadRoot, outputRoot) {
    const capture = readJson(capturePath), build = capture.build_root, source = capture.source_root;
    requireFact(capture.schema === 'singleeditor.clean-build-capture' && capture.version === 1 && capture.phase === 'built',
        'capture_contract_mismatch', capturePath);
    requireFact(!fs.existsSync(outputRoot), 'output_exists', outputRoot);
    for (const root of [build, source, payloadRoot]) requireFact(key(outputRoot) !== key(root)
        && !key(outputRoot).startsWith(key(root) + '/'), 'output_inside_input', outputRoot);
    const full = p => p.startsWith('<BUILD>') ? p.replace('<BUILD>', build) : path.resolve(source, p);
    const normal = p => {
        const absolute = path.resolve(p).replaceAll('\\', '/');
        for (const [root, tag] of [[build, '<BUILD>'], [source, '<SOURCE>']]) {
            const base = path.resolve(root).replaceAll('\\', '/');
            if (absolute.toLowerCase().startsWith(base.toLowerCase() + '/')) return tag + absolute.slice(base.length);
        }
        return absolute;
    };
    for (const entry of capture.inputs.source)
        requireFact(fileFact(path.resolve(source, entry.path)).sha256 === entry.sha256, 'source_changed_since_build', entry.path);
    const cache = fs.readFileSync(path.join(build, 'CMakeCache.txt'), 'utf8');
    for (const [group, variable] of [['juce', 'SINGLE_EDITOR_JUCE_ROOT'], ['asio', 'SINGLE_EDITOR_ASIO_SDK_ROOT'],
        ['samplerate', 'libsamplerate_SOURCE_DIR']]) {
        const root = cache.match(new RegExp(`^${variable}:[^=]+=(.*)$`, 'm'))?.[1].trim();
        requireFact(root, 'dependency_root_missing', variable);
        for (const input of capture.inputs[group])
            requireFact(fileFact(path.join(root, input.path)).sha256 === input.sha256, 'dependency_changed_since_build', input.path);
    }
    for (const input of [...capture.inputs.toolchain, capture.inputs.cmake, ...(capture.inputs.link_evidence ?? [])])
        requireFact(fileFact(input.path).sha256 === input.sha256, 'tool_changed_since_build', input.path);
    const targets = [];
    const wanted = new Set(['SingleEditor', 'SingleEditorCLI']);
    // Follow the actual configured dependency edges to internal compiled targets.
    const byId = new Map(capture.targets.map(t => [t.id, t]));
    const visit = t => {
        for (const dep of t.dependencies ?? []) {
            const next = byId.get(dep.id);
            if (next && !wanted.has(next.name) && ['STATIC_LIBRARY', 'OBJECT_LIBRARY'].includes(next.type)) {
                wanted.add(next.name); visit(next);
            }
        }
    };
    capture.targets.filter(t => wanted.has(t.name)).forEach(visit);
    for (const target of capture.targets.filter(t => wanted.has(t.name))) {
        const targetBuild = path.resolve(build, target.paths.build.replace('<BUILD>', build));
        const intermediate = path.join(targetBuild, target.name + '.dir', 'Release');
        const logs = fs.readdirSync(intermediate).filter(n => n.endsWith('.tlog'))
            .map(n => path.join(intermediate, n)).filter(p =>
                fs.existsSync(path.join(p, 'Cl.items.tlog')) || fs.existsSync(path.join(p, 'rc.write.1.tlog')));
        requireFact(logs.length === 1, 'unsupported_tracking_directory', target.name);
        const log = logs[0], compiled = [], resourceCompiled = [], evidence = [];
        let observerIsolation = null;
        const keepLog = name => {
            const p = path.join(log, name); requireFact(fs.existsSync(p), 'tracking_log_missing', p);
            evidence.push({ path: normal(p), ...fileFact(p) }); return p;
        };
        const itemPath = path.join(log, 'Cl.items.tlog');
        if (fs.existsSync(itemPath)) {
            for (const line of lines(keepLog('Cl.items.tlog'))) {
                const pair = line.split(';'); requireFact(pair.length === 2, 'unsupported_compile_item', line);
                compiled.push({ source: normal(pair[0]), source_fact: fileFact(pair[0]),
                    object: normal(pair[1]), object_fact: fileFact(pair[1]) });
            }
            const commands = trackedRecords(keepLog('CL.command.1.tlog'));
            reconcileSources(compiled.map(s => full(s.source.replace('<SOURCE>', source))), commands.flatMap(c => c.inputs));
            observerIsolation = validateTestObserverIsolation(target.name, target.compileGroups ?? [], commands);
        }
        if (fs.existsSync(path.join(log, 'rc.write.1.tlog'))) {
            for (const record of trackedRecords(keepLog('rc.write.1.tlog'))) {
                requireFact(record.inputs.length === 1, 'unsupported_resource_compilation', target.name);
                const outputs = record.values.filter(p => p.toLowerCase().endsWith('.res'));
                requireFact(outputs.length === 1, 'unsupported_resource_output', target.name);
                resourceCompiled.push({ source: normal(record.inputs[0]), source_fact: fileFact(record.inputs[0]),
                    object: normal(outputs[0]), object_fact: fileFact(outputs[0]) });
            }
            keepLog('rc.command.1.tlog'); keepLog('rc.read.1.tlog');
        }
        const declaredSources = (target.sources ?? []).filter(s => s.compileGroupIndex !== undefined);
        reconcileSources(declaredSources.map(s => full(s.path)),
            [...compiled, ...resourceCompiled].map(s => full(s.source.replace('<SOURCE>', source))));
        targets.push({ name: target.name, type: target.type,
            test_observer_isolation: observerIsolation,
            configured_compile_groups: target.compileGroups, configured_link: target.link ?? target.archive ?? null,
            compiled, resource_compiled: resourceCompiled,
            artifacts: (target.artifacts ?? []).filter(a => !a.path.endsWith('.pdb')).map(a => ({ path: normal(path.resolve(build, a.path)), ...fileFact(path.resolve(build, a.path)) })),
            declared_debug_artifacts: (target.artifacts ?? []).filter(a => a.path.endsWith('.pdb')).map(a => {
                const p = path.resolve(build, a.path);
                return { path: normal(p), present: fs.existsSync(p), ...(fs.existsSync(p) ? fileFact(p) : {}) };
            }),
            tracking_evidence: evidence, log, configured: target });
    }
    const products = [];
    for (const target of targets.filter(t => ['SingleEditor', 'SingleEditorCLI'].includes(t.name))) {
        const built = capture.binaries.find(b => b.target === target.name);
        const executable = path.join(payloadRoot, path.basename(built.path));
        requireFact(fileFact(executable).sha256 === built.sha256 && fileFact(built.path).sha256 === built.sha256,
            'product_hash_mismatch', target.name);
        const linkRead = path.join(target.log, 'link.read.1.tlog'), linkCommand = path.join(target.log, 'link.command.1.tlog');
        const records = trackedRecords(linkRead);
        requireFact(records.length === 1, 'unsupported_link_records', target.name);
        const inputs = [...new Set(records[0].values.map(key))];
        const expectedObjects = new Set([...target.compiled, ...target.resource_compiled].map(o => key(full(o.object))));
        for (const object of [...target.compiled, ...target.resource_compiled])
            requireFact(inputs.includes(key(full(object.object))), 'compiled_output_not_linked', object.object);
        const closure = new Set();
        const gather = t => { for (const d of t.dependencies ?? []) {
            const next = byId.get(d.id); if (next && wanted.has(next.name) && !closure.has(next.name)) { closure.add(next.name); gather(next); }
        } };
        gather(target.configured);
        for (const dependency of targets.filter(t => closure.has(t.name))) {
            const outputs = dependency.type === 'OBJECT_LIBRARY' ? dependency.resource_compiled.map(o => o.object)
                : dependency.artifacts.map(a => a.path);
            for (const p of outputs) requireFact(inputs.includes(key(full(p))), 'declared_dependency_not_linked', p);
            if (dependency.type === 'OBJECT_LIBRARY') for (const p of outputs) expectedObjects.add(key(full(p)));
        }
        for (const p of inputs.filter(p => /\.(obj|res)$/i.test(p)))
            requireFact(expectedObjects.has(p), 'observed_link_object_undeclared', p);
        const facts = peFacts(executable);
        let linkFinalization = null;
        if (capture.inputs.link_evidence) {
            const pointer = path.join(build, 'link-evidence', target.name, 'Release', 'current.json');
            requireFact(fs.existsSync(pointer), 'product_finalization_missing', target.name);
            const record = JSON.parse(fs.readFileSync(pointer, 'utf8'));
            requireFact(record.schema === 'singleeditor.product-link-finalization' && record.version === 1
                && record.target === target.name && record.status === 'build-finalized'
                && record.final_image.sha256 === built.sha256 && key(record.final_image.path) === key(built.path),
                'product_finalization_mismatch', target.name);
            for (const input of [record.linked_image, record.private_pdb, record.finalization,
                ...record.tools, ...record.archived_tools])
                requireFact(fileFact(input.path).sha256 === input.sha256, 'product_finalization_input_changed', input.path);
            const transformed = JSON.parse(fs.readFileSync(record.finalization.path, 'utf8'));
            requireFact(transformed.schema === 'singleeditor.link-evidence-finalization'
                && transformed.status === 'probe-verified' && transformed.output.sha256 === built.sha256
                && transformed.inputs.image.sha256 === record.linked_image.sha256
                && transformed.inputs.pdb.sha256 === record.private_pdb.sha256,
                'product_finalization_chain_mismatch', target.name);
            for (const input of [...Object.values(transformed.inputs), transformed.output, transformed.contributions])
                requireFact(fileFact(input.path).sha256 === input.sha256, 'product_finalization_archive_changed', input.path);
            linkFinalization = { status: 'same-link-finalized', record: { path: normal(pointer), ...fileFact(pointer) },
                finalization: { path: normal(record.finalization.path), ...fileFact(record.finalization.path) },
                contributions: { path: normal(transformed.contributions.path), ...fileFact(transformed.contributions.path) },
                pdb_identity: transformed.pdb_identity, final_image_sha256: built.sha256,
                owner_status: 'physical-origins-observed-semantic-reconciliation-pending' };
        }
        products.push({ target: target.name, payload_path: path.basename(executable), ...facts,
            ...(linkFinalization ? { link_finalization: linkFinalization } : {}),
            link_command: { path: normal(linkCommand), ...fileFact(linkCommand) },
            link_read: { path: normal(linkRead), ...fileFact(linkRead) },
            observed_link_inputs: inputs.map(p => ({ path: normal(p), ...fileFact(p) })),
            owner_contribution: { status: 'unresolved', reason: linkFinalization
                ? 'Exact same-link DIA physical origins are bound to the final file; explicit semantic owner/class and remaining linked ranges still require reconciliation.'
                : 'Compiled object/archive sizes are not final owner byte contributions after LTCG/ICF; no symbol/owner map is available.' } });
        facts.pe_layout.headers.producer_proof = peHeaderProducerProof(executable, facts, capturePath,
            products.at(-1), { build, source, payload: path.resolve(payloadRoot) });
    }
    const carrierDescriptor = path.join(build, 'pe-carrier-intent', 'Release.json');
    const carrierTemplate = path.join(source, 'cmake', 'PeCarrierIntent.json.in');
    let carrierIntent = null;
    if (fs.existsSync(carrierDescriptor) || fs.existsSync(carrierTemplate)) {
        requireFact(fs.existsSync(carrierDescriptor) && fs.existsSync(carrierTemplate),
            'pe_carrier_intent_missing', carrierDescriptor);
        const declared = capture.inputs.source.filter(f => f.path.replaceAll('\\', '/') === 'cmake/PeCarrierIntent.json.in');
        requireFact(declared.length === 1 && declared[0].sha256 === fileFact(carrierTemplate).sha256,
            'pe_carrier_template_capture_mismatch', carrierTemplate);
        const intent = readJson(carrierDescriptor);
        requireFact(intent.schema === 'singleeditor.pe-carrier-intent' && intent.version === 1
            && intent.configuration === 'Release' && intent.architecture === 'x64',
            'pe_carrier_intent_contract', carrierDescriptor);
        carrierIntent = { descriptor: { path: path.resolve(carrierDescriptor), ...fileFact(carrierDescriptor) },
            template: { path: path.resolve(carrierTemplate), ...fileFact(carrierTemplate) },
            source_root: source, build_root: build, configuration: 'Release', architecture: 'x64' };
    }
    fs.mkdirSync(path.dirname(path.resolve(outputRoot)), { recursive: true });
    fs.mkdirSync(outputRoot, { recursive: false });
    const write = (name, value) => fs.writeFileSync(path.join(outputRoot, name), JSON.stringify(value, null, 2) + '\n', { flag: 'wx' });
    const delta = (left, right) => {
        const a = new Set(left), b = new Set(right), order = values => [...values].sort();
        return { shared: order([...a].filter(v => b.has(v))), gui_only: order([...a].filter(v => !b.has(v))),
            cli_only: order([...b].filter(v => !a.has(v))) };
    };
    const symbols = product => product.imports.flatMap(i => i.symbols.map(s =>
        `${i.dll}!${s.name ?? '#' + s.ordinal}`));
    const gui = products.find(p => p.target === 'SingleEditor'), cli = products.find(p => p.target === 'SingleEditorCLI');
    const report = { schema: 'singleeditor.product-build-observation', version: carrierIntent ? 2 : 1, status: 'observed-with-pending',
        ...(carrierIntent ? { pe_carrier_intent: carrierIntent } : {}),
        captured_utc: new Date().toISOString(), capture: { path: path.resolve(capturePath), ...fileFact(capturePath) },
        scanner: { path: 'tools/release/BuildArtifactEvidence.mjs', ...fileFact(new URL(import.meta.url)) },
        roots: { build, source, payload: path.resolve(payloadRoot) },
        targets: targets.map(({ log, configured, ...t }) => t), products,
        variant_delta: {
            direct_compilation_sources: delta(targets.find(t => t.name === 'SingleEditor').compiled.map(s => s.source),
                targets.find(t => t.name === 'SingleEditorCLI').compiled.map(s => s.source)),
            imported_symbols: delta(symbols(gui), symbols(cli)),
            meaning: 'Input/symbol membership only; shared source is not a shared final byte contribution.' },
        pending: ['semantic owner/lifecycle descriptors and final linked owner slices',
            'full command-option reconciliation, link-map/symbol attribution, all implicit compiler includes and complete toolchain/environment provenance',
            'calibrated owner/class absolute/delta/ratchet budgets',
            'resource ownership/pack contracts and embedded non-PE assets',
            'signature trust, installer variant, runtime trigger evidence and final fixed-path gates'],
        release_eligible: false };
    write('observation.json', report);
    return report;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
    try {
        const args = process.argv.slice(2), options = {};
        requireFact(args.length === 6, 'invalid_arguments', '--capture <built.json> --payload <root> --output <new-directory>');
        for (let i = 0; i < args.length; i += 2) {
            requireFact(['--capture', '--payload', '--output'].includes(args[i]) && !options[args[i]], 'invalid_arguments', args[i]);
            options[args[i]] = args[i + 1];
        }
        const result = observeBuild(options['--capture'], options['--payload'], options['--output']);
        console.log(JSON.stringify({ status: result.status, targets: result.targets.map(t => ({ name: t.name, compiled: t.compiled.length, resources: t.resource_compiled.length })),
            products: result.products.map(p => ({ target: p.target, sections: p.sections.length, imports: p.imports.length,
                imported_symbols: p.imports.reduce((n, i) => n + i.symbols.length, 0), resources: p.resources.length, bytes: p.bytes })),
            release_eligible: false }));
    } catch (error) { console.error(error.message); process.exitCode = 1; }
}
