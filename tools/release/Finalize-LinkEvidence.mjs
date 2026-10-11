// ToolBuild probe only. Derive a new image from one private Release link while
// retaining its own PDB identity and exact runtime address/byte evidence.
// This command is not yet a product CMake step or release approval.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { peFacts } from './BuildArtifactEvidence.mjs';

const digest = b => crypto.createHash('sha256').update(b).digest('hex').toUpperCase();
const fact = p => ({ path: path.resolve(p), bytes: fs.statSync(p).size, sha256: digest(fs.readFileSync(p)) });
const requireFact = (ok, message) => { if (!ok) throw new Error(message); };
const [imagePath, pdbPath, inspectorPath, diaPath, outputPath] = process.argv.slice(2);
try {
    requireFact(outputPath, 'Usage: <linked.exe> <matching.pdb> <inspector.exe> <msdia140.dll> <new-output>');
    const output = path.resolve(outputPath);
    requireFact(!fs.existsSync(output), 'Output already exists; preserve the previous image and evidence.');
    const inputs = { image: fact(imagePath), pdb: fact(pdbPath), inspector: fact(inspectorPath),
        dia: fact(diaPath), finalizer: fact(fileURLToPath(import.meta.url)) };
    const original = fs.readFileSync(imagePath), before = peFacts(imagePath), result = Buffer.from(original);
    const pe = original.readUInt32LE(0x3c), optional = pe + 24;
    requireFact(original.readUInt16LE(pe + 4) === 0x8664 && original.readUInt16LE(optional) === 0x20b,
        'Only the observed AMD64 PE32+ Release layout is supported.');
    requireFact(original.readUInt32LE(optional + 64) === 0 && original.readUInt32LE(optional + 112 + 4 * 8) === 0
        && original.readUInt32LE(optional + 116 + 4 * 8) === 0, 'Select an unsigned, zero-checksum build intermediate.');
    const locate = (rva, length) => {
        const section = before.sections.find(s => rva >= s.rva && rva + length <= s.rva + Math.min(s.raw_bytes, s.virtual_bytes));
        requireFact(section, `Unmapped metadata range ${rva}+${length}`);
        return { section, offset: section.file_offset + rva - section.rva };
    };
    const debugSizeOffset = optional + 116 + 6 * 8;
    const tableRva = original.readUInt32LE(optional + 112 + 6 * 8), tableSize = original.readUInt32LE(debugSizeOffset);
    requireFact(tableSize > 0 && tableSize % 28 === 0, 'Expected a complete linked debug directory.');
    const table = locate(tableRva, tableSize), records = [];
    for (let offset = table.offset; offset < table.offset + tableSize; offset += 28) {
        const type = original.readUInt32LE(offset + 12), bytes = original.readUInt32LE(offset + 16);
        const rva = original.readUInt32LE(offset + 20), file = original.readUInt32LE(offset + 24);
        requireFact([2, 12, 13, 16].includes(type), `Review unsupported linked debug type ${type}.`);
        if (bytes) requireFact(locate(rva, bytes).offset === file, 'Debug file offset and RVA disagree.');
        records.push({ type, bytes, rva, file, directory: Buffer.from(original.subarray(offset, offset + 28)) });
    }
    const codeView = records.filter(r => r.type === 2);
    requireFact(codeView.length === 1 && codeView[0].bytes > 24, 'Exactly one RSDS PDB reference required.');
    const cv = original.subarray(codeView[0].file, codeView[0].file + codeView[0].bytes);
    requireFact(cv.toString('ascii', 0, 4) === 'RSDS' && cv[cv.length - 1] === 0, 'Unsupported CodeView record.');
    const guid = `{${cv.readUInt32LE(4).toString(16).padStart(8, '0')}-${cv.readUInt16LE(8).toString(16).padStart(4, '0')}-${cv.readUInt16LE(10).toString(16).padStart(4, '0')}-${cv.subarray(12, 14).toString('hex')}-${cv.subarray(14, 20).toString('hex')}}`.toUpperCase();
    const age = cv.readUInt32LE(20);
    const rawContributions = execFileSync(path.resolve(inspectorPath), [path.resolve(diaPath), path.resolve(pdbPath)],
        { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 });
    const contributions = JSON.parse(rawContributions);
    requireFact(contributions.schema === 'singleeditor.dia-section-contributions' && contributions.version === 2
        && contributions.identity.guid.toUpperCase() === guid && contributions.identity.age === age,
        'PDB GUID/age does not match this exact linked image.');
    requireFact(contributions.count > 0 && contributions.count === contributions.contributions.length,
        'Complete actual DIA contribution table required.');
    const removed = records.filter(r => [2, 12].includes(r.type)), retained = records.filter(r => ![2, 12].includes(r.type));
    const edits = [{ offset: debugSizeOffset, bytes: 4, reason: 'debug-directory-size' },
        { offset: table.offset, bytes: tableSize, reason: 'debug-directory-records' },
        ...removed.filter(r => r.bytes).map(r => ({ offset: r.file, bytes: r.bytes, reason: `debug-type-${r.type}-data` }))];
    requireFact(table.section.name === '.rdata' && (table.section.characteristics & 0xa0000000) === 0,
        'Debug directory must be read-only, non-executable rdata.');
    // The transformation may touch only debug metadata, never another PE table.
    const directories = original.readUInt32LE(optional + 108);
    requireFact(directories <= 16, 'Review additional PE data directories.');
    for (let i = 0; i < directories; ++i) if (i !== 4 && i !== 6) {
        const rva = original.readUInt32LE(optional + 112 + i * 8);
        const bytes = original.readUInt32LE(optional + 116 + i * 8);
        if (!bytes) continue;
        const file = locate(rva, bytes).offset;
        for (const edit of edits.slice(1))
            requireFact(edit.offset + edit.bytes <= file || file + bytes <= edit.offset,
                `Debug finalization overlaps PE data directory ${i}.`);
    }
    for (const row of removed) if (row.bytes) {
        const metadata = locate(row.rva, row.bytes).section;
        requireFact(metadata.name === '.rdata' && (metadata.characteristics & 0xa0000000) === 0,
            'Removed metadata must be read-only, non-executable rdata.');
        for (const peer of records.filter(r => r !== row && r.bytes))
            requireFact(row.file + row.bytes <= peer.file || peer.file + peer.bytes <= row.file, 'Debug data ranges overlap.');
        requireFact(row.file + row.bytes <= table.offset || table.offset + tableSize <= row.file, 'Debug data overlaps its directory.');
        result.fill(0, row.file, row.file + row.bytes);
    }
    result.fill(0, table.offset, table.offset + tableSize);
    retained.forEach((r, i) => r.directory.copy(result, table.offset + i * 28));
    result.writeUInt32LE(retained.length * 28, debugSizeOffset);
    edits.sort((a, b) => a.offset - b.offset);
    let cursor = 0, unchangedBytes = 0;
    for (const edit of edits) {
        requireFact(edit.offset >= cursor && edit.offset + edit.bytes <= original.length, 'Finalization ranges overlap or exceed the image.');
        requireFact(original.subarray(cursor, edit.offset).equals(result.subarray(cursor, edit.offset)), 'Non-debug bytes changed.');
        unchangedBytes += edit.offset - cursor; cursor = edit.offset + edit.bytes;
    }
    requireFact(original.subarray(cursor).equals(result.subarray(cursor)), 'Non-debug tail changed.');
    unchangedBytes += original.length - cursor;
    const sections = before.sections.map(s => ({ name: s.name, rva: s.rva, virtual_bytes: s.virtual_bytes, raw_bytes: s.raw_bytes,
        original_sha256: digest(original.subarray(s.file_offset, s.file_offset + s.raw_bytes)),
        final_sha256: digest(result.subarray(s.file_offset, s.file_offset + s.raw_bytes)) }));
    requireFact(sections.filter(s => s.name !== '.rdata').every(s => s.original_sha256 === s.final_sha256),
        'Code, runtime data, exception/resource or relocation bytes changed.');
    const transformedContributions = [];
    for (const row of contributions.contributions) {
        const section = before.sections[row.section - 1];
        requireFact(section && row.rva === section.rva + row.offset && row.offset + row.bytes <= Math.max(section.virtual_bytes, section.raw_bytes),
            'DIA contribution does not fit the linked image layout.');
        const rawBytes = Math.max(0, Math.min(row.bytes, section.raw_bytes - row.offset));
        const file = section.file_offset + row.offset;
        const touched = edits.filter(e => rawBytes && e.offset < file + rawBytes && file < e.offset + e.bytes);
        if (touched.length) transformedContributions.push({ ...row, edits: touched });
    }
    for (const input of Object.values(inputs)) requireFact(fact(input.path).sha256 === input.sha256, 'A finalization input changed.');
    fs.mkdirSync(output, { recursive: true });
    const finalPath = path.join(output, path.basename(imagePath));
    fs.writeFileSync(finalPath, result, { flag: 'wx' });
    const after = peFacts(finalPath);
    requireFact(!after.debug_types.some(t => [2, 12].includes(t)), 'Final image retains removed debug entries.');
    fs.writeFileSync(path.join(output, 'contributions.json'), rawContributions, { flag: 'wx' });
    const receipt = { schema: 'singleeditor.link-evidence-finalization', version: 1, status: 'probe-verified', release_eligible: false,
        inputs, pdb_identity: { guid, age }, output: fact(finalPath), contributions: fact(path.join(output, 'contributions.json')),
        edits, transformed_contributions: transformedContributions,
        unchanged_bytes: unchangedBytes, unchanged_runtime_layout: true, sections, final_debug_types: after.debug_types,
        scope: 'One private Release link and its exact PDB. Only declared debug metadata is removed; runtime code/addresses and all other bytes are unchanged. Not integrated into product CMake, not a full owner-budget or release verdict.' };
    fs.writeFileSync(path.join(output, 'receipt.json'), JSON.stringify(receipt, null, 2) + '\n', { flag: 'wx' });
    console.log(JSON.stringify({ status: receipt.status, output: receipt.output, pdb_identity: receipt.pdb_identity,
        contributions: contributions.count, final_debug_types: after.debug_types, unchanged_bytes: unchangedBytes }));
} catch (error) { console.error(error.message); process.exitCode = 1; }
