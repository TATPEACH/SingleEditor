// ToolBuild-only: witness fresh product outputs, stable inputs/configuration and
// raw GUI/CLI reproducibility. Full owner/BOM and release gates remain separate.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const read = p => JSON.parse(fs.readFileSync(p, 'utf8'));
const hash = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex').toUpperCase();
const slash = p => p.replaceAll('\\', '/');
function writeNew(p, value) {
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, JSON.stringify(value, null, 2) + '\n', { flag: 'wx' });
}
function requireThat(condition, message) { if (!condition) throw new Error(message); }
function configuredDependencyRoot(text, keys, source, label) {
    const declared = keys.map(key => text.match(new RegExp(`^${key}:[^=]+=(.*)$`, 'm'))?.[1].trim()).find(Boolean);
    requireThat(declared && !/[;$\r\n]/.test(declared) && !declared.endsWith('-NOTFOUND'),
        `${label}: unresolved dependency root; configure an explicit or resolved provider cache key.`);
    const root = path.resolve(source, declared);
    requireThat(fs.existsSync(root) && fs.statSync(root).isDirectory(), `${label}: missing dependency root ${root}`);
    return root;
}
function files(root) {
    return fs.readdirSync(root, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name)).flatMap(e => {
        const p = path.join(root, e.name);
        if (e.name === '.git') return [];
        return e.isDirectory() ? files(p) : e.isFile() ? [p] : [];
    });
}
function inventory(root, entries) {
    return entries.flatMap(e => {
        const p = path.join(root, e);
        return fs.statSync(p).isDirectory() ? files(p) : [p];
    }).map(p => ({ path: slash(path.relative(root, p)), sha256: hash(p) }));
}
function normalise(value, build) {
    if (typeof value === 'string') return value.replaceAll(slash(build), '<BUILD>').replaceAll(build, '<BUILD>');
    if (Array.isArray(value)) return value.map(v => normalise(v, build));
    if (value && typeof value === 'object') return Object.fromEntries(Object.keys(value).sort()
        .filter(k => !['backtrace', 'backtraces', 'backtraceGraph'].includes(k))
        .map(k => [k, normalise(value[k], build)]));
    return value;
}
function model(build) {
    const replies = path.join(build, '.cmake/api/v1/reply');
    const name = fs.readdirSync(replies).filter(n => /^index-.*\.json$/.test(n)).sort().at(-1);
    requireThat(name, 'A configured codemodel-v2 query is required.');
    const index = read(path.join(replies, name));
    const code = read(path.join(replies, index.reply['codemodel-v2'].jsonFile));
    requireThat(path.resolve(code.paths.build) === build, 'File API build root mismatch.');
    const release = code.configurations.find(c => c.name === 'Release');
    requireThat(release, 'Release configuration missing.');
    const targets = release.targets.map(t => normalise(read(path.join(replies, t.jsonFile)), build));
    // Directory-scoped CMake ids contain opaque hashes of the build path.
    // Compare graph identities by target + normalised source/build directory.
    const ids = new Map(targets.map(t => [t.id, `${t.name}@${t.paths.source}@${t.paths.build}`]));
    for (const t of targets) {
        t.id = ids.get(t.id);
        for (const key of ['dependencies', 'orderDependencies']) if (t[key]) {
            // Configure-built imported tools (juceaide) need not be codemodel
            // targets. Retain their exact ids; do not silently drop the edge.
            for (const d of t[key]) d.id = ids.get(d.id) ?? `external:${d.id}`;
            t[key].sort((a, b) => a.id.localeCompare(b.id));
        }
    }
    targets.sort((a, b) => a.id.localeCompare(b.id));
    return { source: code.paths.source, generator: index.cmake, targets };
}
function peFacts(filename) {
    const b = fs.readFileSync(filename), pe = b.readUInt32LE(0x3c), optional = pe + 24;
    requireThat(b.toString('ascii', pe, pe + 4) === 'PE\0\0', 'Expected a PE executable.');
    requireThat(b.readUInt16LE(optional) === 0x20b, 'Expected PE32+ Release artifact.');
    const sections = [];
    for (let i = 0, p = optional + b.readUInt16LE(pe + 20); i < b.readUInt16LE(pe + 6); ++i, p += 40)
        sections.push({ rva: b.readUInt32LE(p + 12), bytes: b.readUInt32LE(p + 16), offset: b.readUInt32LE(p + 20) });
    const rva = b.readUInt32LE(optional + 112 + 6 * 8), size = b.readUInt32LE(optional + 116 + 6 * 8);
    const section = sections.find(s => rva >= s.rva && rva + size <= s.rva + s.bytes);
    requireThat(section && size % 28 === 0, 'Expected a mapped PE debug directory.');
    const types = [];
    for (let p = section.offset + rva - section.rva, end = p + size; p < end; p += 28) types.push(b.readUInt32LE(p + 12));
    return { bytes: b.length, sha256: hash(filename), machine: b.readUInt16LE(pe + 4), debug_types: types,
        reproducible: types.includes(16), codeview: types.includes(2) };
}
function capture(build, phase) {
    build = path.resolve(build);
    requireThat(['prepared', 'built'].includes(phase), 'Use phase prepared or built.');
    const m = model(build), source = path.resolve(m.source);
    const text = fs.readFileSync(path.join(build, 'CMakeCache.txt'), 'utf8');
    const cache = key => {
        const match = text.match(new RegExp(`^${key}:[^=]+=(.*)$`, 'm'));
        requireThat(match, `Missing cache input ${key}`); return match[1].trim();
    };
    for (const key of ['SINGLE_EDITOR_ENABLE_OPEN_TIMING', 'SINGLE_EDITOR_DOCUMENT_TEST_ASAN'])
        requireThat(cache(key) === 'OFF', `${key} must be OFF.`);
    const compilerFile = path.join(build, 'CMakeFiles', m.generator.version.string, 'CMakeCXXCompiler.cmake');
    const compilerText = fs.readFileSync(compilerFile, 'utf8');
    const compiler = compilerText.match(/set\(CMAKE_CXX_COMPILER "([^"]+)"/)[1];
    const compilerDir = path.dirname(compiler);
    const inputs = {
        source: inventory(source, ['CMakeLists.txt', 'Source', 'Tests', 'Resources', 'cmake']),
        juce: inventory(configuredDependencyRoot(text, ['SINGLE_EDITOR_RESOLVED_JUCE_ROOT', 'SINGLE_EDITOR_JUCE_ROOT'], source, 'JUCE'), ['CMakeLists.txt', 'modules', 'extras/Build']),
        asio: inventory(configuredDependencyRoot(text, ['SINGLE_EDITOR_RESOLVED_ASIO_SDK_ROOT', 'SINGLE_EDITOR_ASIO_SDK_ROOT'], source, 'ASIO SDK'), ['.']),
        samplerate: inventory(cache('libsamplerate_SOURCE_DIR'), ['.']),
        toolchain: ['cl.exe', 'link.exe', 'lib.exe', 'c1.dll', 'c1xx.dll', 'c2.dll']
            .map(n => ({ path: slash(path.join(compilerDir, n)), sha256: hash(path.join(compilerDir, n)) })),
        cmake: { path: m.generator.paths.cmake, sha256: hash(m.generator.paths.cmake) },
        compiler: normalise(compilerText, build),
        source_epoch: cache('SINGLE_EDITOR_SOURCE_DATE_EPOCH'),
    };
    if (text.match(/^SINGLE_EDITOR_ENABLE_LINK_EVIDENCE:[^=]+=(.*)$/m)?.[1].trim() === 'ON') {
        const releaseTools = path.resolve(source, '../tools/release');
        const diaRoot = cache('SINGLE_EDITOR_DIA_ROOT');
        inputs.link_evidence = [
            ...['Finalize-ProductLink.mjs', 'Finalize-LinkEvidence.mjs', 'BuildArtifactEvidence.mjs',
                'dia/DiaSectionContributions.cpp', 'dia/CMakeLists.txt'].map(p => path.join(releaseTools, p)),
            ...['include/dia2.h', 'lib/amd64/diaguids.lib', 'bin/amd64/msdia140.dll'].map(p => path.join(diaRoot, p)),
            cache('SINGLE_EDITOR_NODE_EXECUTABLE')
        ].map(p => ({ path: slash(p), sha256: hash(p) }));
    }
    const binaries = ['SingleEditor', 'SingleEditorCLI'].map(name => {
        const t = m.targets.find(t => t.name === name), relative = t?.artifacts?.find(a => a.path.endsWith('.exe'))?.path;
        requireThat(relative, `Missing product ${name}.`);
        const p = path.resolve(build, relative);
        requireThat(t.link.commandFragments.some(f => f.fragment.includes('/Brepro')), `${name}: /Brepro missing.`);
        requireThat(t.compileGroups.some(g => {
            const flags = (g.compileCommandFragments ?? []).map(f => f.fragment).join(' ');
            return flags.includes('/experimental:deterministic') && flags.includes('/pathmap:');
        }),
            `${name}: deterministic compilation/path mapping missing.`);
        if (phase === 'prepared') {
            requireThat(!fs.existsSync(p), `Prepared capture refuses existing executable: ${p}`);
            const dir = path.join(build, name + '.dir');
            requireThat(!fs.existsSync(dir) || !files(dir).some(p => /\.(obj|pch|idb|iobj|ipdb)$/i.test(p)),
                `Prepared capture refuses existing product compiler outputs: ${name}`);
            return { target: name, path: p, absent_before_build: true };
        }
        const facts = peFacts(p);
        requireThat(facts.reproducible && !facts.codeview && facts.machine === 0x8664,
            `${name}: expected x64 reproducible PE without CodeView debug payload.`);
        return { target: name, path: p, ...facts };
    });
    return { schema: 'singleeditor.clean-build-capture', version: 1, phase, captured_utc: new Date().toISOString(),
        build_root: build, source_root: source,
        head: execFileSync('git', ['rev-parse', 'HEAD'], { cwd: source, encoding: 'utf8' }).trim(),
        inputs, generator: m.generator, targets: m.targets, binaries,
        scope: 'Fresh GUI/CLI product outputs and observed Release configuration. Configure-built juceaide is a build tool. Full owner attribution, installer, sealed BOM, fixed-path and RC gates are separate.' };
}
function firstDifference(a, b, route = '') {
    if (Object.is(a, b)) return null;
    if (!a || !b || typeof a !== 'object' || typeof b !== 'object') return route;
    if (Array.isArray(a) !== Array.isArray(b)) return route;
    if (Array.isArray(a) && a.length !== b.length) return `${route}.length`;
    for (const key of [...new Set([...Object.keys(a), ...Object.keys(b)])].sort()) {
        if (!(key in a) || !(key in b)) return `${route}.${key}`;
        const d = firstDifference(a[key], b[key], `${route}.${key}`); if (d !== null) return d;
    }
    return null;
}
function comparableTargets(targets) {
    const result = structuredClone(targets);
    for (const target of result) {
        const aliases = new Set();
        for (const source of target.sources ?? []) {
            // CMake's synthetic .rule directory hash encodes its output root.
            // Keep the actual snapshots; normalise only generated rule aliases,
            // never compiled/generated source bytes or their ordering.
            if (source.isGenerated && /^<BUILD>\/CMakeFiles\/[a-f0-9]{32}\/[^/]+\.rule$/.test(source.path)) {
                source.path = source.path.replace(/\/CMakeFiles\/[a-f0-9]{32}\//, '/CMakeFiles/<RULE-ID>/');
                requireThat(!aliases.has(source.path), 'Ambiguous generated rule alias.');
                aliases.add(source.path);
            }
        }
    }
    return result;
}
function compareCaptures(paths) {
    const names = ['--prepared-a', '--built-a', '--prepared-b', '--built-b'];
    requireThat(paths.length === 4, 'Exactly four prepared/built captures required.');
    const values = paths.map(read);
    values.forEach((v, i) => requireThat(v.schema === 'singleeditor.clean-build-capture' && v.version === 1
        && v.phase === (i % 2 ? 'built' : 'prepared'), 'Capture schema/phase mismatch.'));
    requireThat(path.resolve(values[0].build_root).toLowerCase() !== path.resolve(values[2].build_root).toLowerCase(),
        'Two independent build directories required.');
    const differences = [];
    for (const [a, b] of [[0, 1], [2, 3], [1, 3]]) {
        if (a % 2 === 0) requireThat(values[a].build_root === values[b].build_root, 'Prepared/built root mismatch.');
        for (const key of ['source_root', 'head', 'inputs', 'generator', 'targets']) {
            const left = key === 'targets' ? comparableTargets(values[a][key]) : values[a][key];
            const right = key === 'targets' ? comparableTargets(values[b][key]) : values[b][key];
            const d = firstDifference(left, right, key);
            if (d !== null) differences.push(`${names[a]}:${names[b]}:${d}`);
        }
    }
    const binaries = v => v.binaries.map(({ path: ignored, ...facts }) => facts);
    const d = firstDifference(binaries(values[1]), binaries(values[3]), 'binaries'); if (d !== null) differences.push(d);
    return { schema: 'singleeditor.clean-build-comparison', version: 1, status: differences.length ? 'failed' : 'pass', differences,
        inputs: paths.map(p => ({ path: path.resolve(p), sha256: hash(p) })), binaries: binaries(values[3]),
        scope: 'Unmodified raw PE byte identity, unchanged source/dependency/tool inputs and equivalent ordered Release configuration. This does not clear full distribution/RC gates.' };
}
function verifyComparison(receiptPath, payloadRoot) {
    const before = hash(receiptPath), receipt = read(receiptPath);
    requireThat(receipt.schema === 'singleeditor.clean-build-comparison' && receipt.version === 1
        && receipt.status === 'pass' && receipt.differences.length === 0 && receipt.inputs.length === 4,
        'A passing four-capture clean comparison is required.');
    for (const item of receipt.inputs)
        requireThat(hash(item.path) === item.sha256, `Referenced capture changed: ${item.path}`);
    const paths = receipt.inputs.map(item => item.path), recomputed = compareCaptures(paths);
    requireThat(firstDifference(receipt, recomputed) === null, 'Retained comparison does not match recomputed capture facts.');
    const captures = paths.map(read), productNames = ['SingleEditor', 'SingleEditorCLI'];
    for (let i = 0; i < captures.length; ++i) {
        const capture = captures[i];
        requireThat(firstDifference(capture.binaries.map(item => item.target), productNames) === null,
            'Exactly GUI and CLI products are required.');
        for (const item of capture.binaries) {
            const relative = path.relative(capture.build_root, item.path);
            requireThat(relative && !relative.startsWith('..') && !path.isAbsolute(relative), 'Product is outside its captured build.');
            if (i % 2 === 0) {
                requireThat(item.absent_before_build === true, 'Prepared capture lacks fresh product observation.');
                requireThat(item.path === captures[i + 1].binaries.find(b => b.target === item.target).path,
                    'Prepared/built product paths differ.');
            } else {
                const { target, path: ignored, ...expected } = item;
                requireThat(firstDifference(peFacts(item.path), expected) === null, `Retained build product changed: ${target}`);
            }
        }
    }
    const products = recomputed.binaries.map(item => {
        const filename = item.target === 'SingleEditor' ? 'Single Editor.exe' : 'SingleEditorCLI.exe';
        const actualPath = path.resolve(payloadRoot, filename), actual = peFacts(actualPath);
        const { target, ...expected } = item;
        requireThat(firstDifference(actual, expected) === null, `Selected payload product differs from clean rebuild: ${target}`);
        return { target, path: actualPath, ...actual };
    });
    requireThat(hash(receiptPath) === before && receipt.inputs.every(item => hash(item.path) === item.sha256),
        'Clean rebuild evidence changed during verification.');
    return { schema: 'singleeditor.clean-build-verification', version: 1, status: 'pass',
        comparison: { path: path.resolve(receiptPath), sha256: before }, captures: receipt.inputs,
        build_roots: [captures[1].build_root, captures[3].build_root], head: captures[1].head,
        verifier: { path: fileURLToPath(import.meta.url), sha256: hash(fileURLToPath(import.meta.url)) },
        products, scope: 'Read-only recomparison of retained captures and actual GUI/CLI PE bytes in both builds and the selected payload. Full package/owner reconciliation, installed integration and RC remain separate.' };
}
try {
    const [mode, ...args] = process.argv.slice(2), options = {};
    requireThat(args.length % 2 === 0, 'Arguments must be --name value pairs.');
    for (let i = 0; i < args.length; i += 2) { requireThat(args[i].startsWith('--') && !(args[i] in options), 'Invalid option.'); options[args[i]] = args[i + 1]; }
    const opt = k => { requireThat(options[k], `Missing ${k}`); return options[k]; };
    if (mode === 'capture') {
        const result = capture(opt('--build'), opt('--phase')); writeNew(opt('--output'), result);
        console.log(JSON.stringify({ status: 'captured', phase: result.phase, targets: result.targets.length,
            input_counts: Object.fromEntries(Object.entries(result.inputs).filter(([, v]) => Array.isArray(v)).map(([k, v]) => [k, v.length])), binaries: result.binaries }));
    } else if (mode === 'compare') {
        const names = ['--prepared-a', '--built-a', '--prepared-b', '--built-b'];
        const result = compareCaptures(names.map(opt));
        writeNew(opt('--output'), result); console.log(JSON.stringify(result)); process.exitCode = result.status === 'pass' ? 0 : 1;
    } else if (mode === 'verify') {
        console.log(JSON.stringify(verifyComparison(opt('--comparison'), opt('--payload'))));
    } else if (mode === 'prepare-source') {
        requireThat(Object.keys(options).every(k => ['--source', '--destination', '--build'].includes(k)), 'Unknown prepare-source option.');
        const { prepareSourceReplay } = await import('./SourceReplayPreparation.mjs');
        console.log(JSON.stringify(prepareSourceReplay({ sourceRoot: opt('--source'), destinationRoot: opt('--destination'),
            buildRoot: options['--build'] ?? null, entrypointPath: fileURLToPath(import.meta.url) })));
    } else if (mode === 'verify-source') {
        requireThat(Object.keys(options).every(k => k === '--manifest'), 'Unknown verify-source option.');
        const { verifySourceReplay } = await import('./SourceReplayPreparation.mjs');
        console.log(JSON.stringify(verifySourceReplay(opt('--manifest'))));
    } else throw new Error('Use capture, compare, verify, prepare-source or verify-source.');
} catch (error) { console.error(error.message); process.exitCode = 1; }
