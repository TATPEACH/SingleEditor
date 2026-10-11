// Focused source replay fixtures only: no configure, compiler or product run.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import vm from 'node:vm';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { prepareSourceReplay, verifySourceReplay, resolveConfiguredDependencyRoot } from '../release/SourceReplayPreparation.mjs';

const entrypoint = fileURLToPath(new URL('../release/Compare-CleanBuilds.mjs', import.meta.url));
const output = process.argv[2] ? path.resolve(process.argv[2]) : fs.mkdtempSync(path.join(os.tmpdir(), 'singleeditor-source-replay-'));
if (process.argv[2]) fs.mkdirSync(output); // A retained test run is never overwritten.
const cases = [];
const put = (p, value) => { fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, value); };
const read = p => JSON.parse(fs.readFileSync(p, 'utf8'));
const iconHeader = 'Source/UI/LookAndFeel/ModernNorthStarVisualContract.h';
function fixture(name) {
    const base = path.join(output, name), repository = path.join(base, 'repo'), source = path.join(repository, 'SingleEditorJuce');
    for (const dir of ['Source', 'Tests', 'Resources', 'cmake']) fs.mkdirSync(path.join(source, dir), { recursive: true });
    put(path.join(source, 'CMakeLists.txt'), 'project(SourceReplayFixture)\n');
    put(path.join(source, 'Source/Untracked.cpp'), 'int sourceReplayFixture = 1;\n');
    put(path.join(source, 'Tests/Fixture.cpp'), '// fixture\n');
    put(path.join(source, 'Resources/sample.bin'), Buffer.from([0, 1, 2, 255]));
    put(path.join(source, 'cmake/UntrackedHelper.cmake'), '# untracked fixture\n');
    const icons = Array.from({ length: 21 }, (_, i) => String.fromCharCode(65 + i));
    put(path.join(source, iconHeader), icons.map(id => `case ModernNorthStarAssetId::${id}: return { id, "icon-${id}.svg", ModernNorthStarAssetOwner::core };`).join('\n'));
    for (const id of icons) put(path.join(repository, `icons/icon-${id}.svg`), `<svg id="${id}"/>\n`);
    for (const p of ['Finalize-ProductLink.mjs', 'Finalize-LinkEvidence.mjs', 'BuildArtifactEvidence.mjs',
        'dia/DiaSectionContributions.cpp', 'dia/CMakeLists.txt', 'ExtraReleaseHelper.mjs']) put(path.join(repository, 'tools/release', p), '// ' + p + '\n');
    put(path.join(repository, 'icons/not-declared.svg'), '<svg/>');
    put(path.join(repository, 'out/not-an-input.bin'), 'do not copy');
    return { base, repository, source, destination: path.join(base, 'copy') };
}
function cli(args, expectedStatus = 0) {
    const result = spawnSync(process.execPath, [entrypoint, ...args], { encoding: 'utf8', timeout: 30000 });
    assert.equal(result.status, expectedStatus, result.stderr || result.error?.message || result.stdout);
    return result.stdout.trim() ? JSON.parse(result.stdout) : null;
}
function prepare(f, extra = {}) { return prepareSourceReplay({ sourceRoot: f.source, destinationRoot: f.destination, entrypointPath: entrypoint, ...extra }); }
function failedCopy(f, pattern) {
    assert.throws(() => prepare(f), pattern);
    assert.equal(fs.existsSync(path.join(f.destination, 'source-replay.json')), false);
    assert.equal(read(path.join(f.destination, 'source-replay-failure.json')).partial_copy_retained, true);
}
function test(name, run) { run(); cases.push({ name, status: 'pass' }); }

test('CLI copies current declared membership and reports external roots only as declarations', () => {
    const f = fixture('success'), build = path.join(f.base, 'build'), dependency = path.join(f.base, 'external-juce');
    fs.mkdirSync(dependency); put(path.join(dependency, 'external.bin'), 'never copied');
    put(path.join(build, 'CMakeCache.txt'), [`CMAKE_HOME_DIRECTORY:INTERNAL=${f.source}`,
        `SINGLE_EDITOR_RESOLVED_JUCE_ROOT:INTERNAL=${dependency}`, 'SINGLE_EDITOR_JUCE_ROOT:PATH=',
        'SINGLE_EDITOR_ASIO_SDK_ROOT:PATH=', `libsamplerate_SOURCE_DIR:STATIC=${dependency}`].join('\n'));
    const result = cli(['prepare-source', '--source', f.source, '--destination', f.destination, '--build', build]);
    assert.equal(result.status, 'prepared');
    const manifest = read(result.manifest.path), names = manifest.files.map(f => f.logical_path);
    assert.equal(manifest.selection.icons.length, 21);
    for (const p of ['SingleEditorJuce/Source/Untracked.cpp', 'SingleEditorJuce/cmake/UntrackedHelper.cmake',
        'tools/release/ExtraReleaseHelper.mjs', 'icons/icon-A.svg', 'icons/icon-U.svg']) assert(names.includes(p), p);
    assert(!names.some(p => p.includes('not-an-input') || p.includes('not-declared') || p.includes('external.bin') || p.startsWith('.git/')));
    assert.equal(manifest.external_dependencies.roots[0].selected_key, 'SINGLE_EDITOR_RESOLVED_JUCE_ROOT');
    assert.equal(manifest.external_dependencies.roots[1].status, 'unresolved');
    assert.equal(manifest.external_dependencies.content_verified, false);
    for (const flag of ['git_clean_checkout', 'complete_build_inputs', 'build_performed', 'binary_reproducibility_proven', 'release_eligible']) assert.equal(manifest[flag], false);
    assert.equal(manifest.producers.length, 2);
    assert.equal(cli(['verify-source', '--manifest', result.manifest.path]).status, 'pass');
    // Retained copy verification remains usable after the working tree evolves.
    put(path.join(f.source, 'Source/Untracked.cpp'), 'later source edit\n');
    assert.equal(verifySourceReplay(result.manifest.path).status, 'pass');
});

test('missing declared input leaves an explicit retained failure', () => {
    const icon = fixture('missing-icon'); fs.unlinkSync(path.join(icon.repository, 'icons/icon-A.svg'));
    failedCopy(icon, /missing_path/);
    const tool = fixture('missing-tool'); fs.unlinkSync(path.join(tool.repository, 'tools/release/Finalize-ProductLink.mjs'));
    failedCopy(tool, /missing_release_tool/);
});

test('retained copy byte tamper and extra membership are rejected', () => {
    const bytes = fixture('copy-tamper'), byteResult = prepare(bytes), file = path.join(bytes.destination, 'SingleEditorJuce/Resources/sample.bin');
    put(file, Buffer.from([0, 1, 3, 255]));
    assert.throws(() => verifySourceReplay(byteResult.manifest.path), /copied_manifest_content_changed/);
    const member = fixture('copy-extra'), memberResult = prepare(member);
    put(path.join(member.destination, 'unexpected.bin'), 'extra');
    assert.throws(() => verifySourceReplay(memberResult.manifest.path), /copied_file_membership_changed/);
});

test('source byte or membership mutation during copying is rejected', () => {
    for (const mutation of ['bytes', 'membership']) {
        const f = fixture('source-change-' + mutation), copyFile = fs.copyFileSync;
        let changed = false;
        try {
            fs.copyFileSync = (source, target, flags) => {
                copyFile(source, target, flags);
                if (!changed) {
                    changed = true;
                    if (mutation === 'bytes') fs.appendFileSync(source, '\nchanged after this file was copied');
                    else put(path.join(f.source, 'Source/NewDuringCopy.cpp'), '// newly added input\n');
                }
            };
            failedCopy(f, /source_bytes_or_membership_changed/);
        } finally { fs.copyFileSync = copyFile; }
        assert(changed);
    }
});

test('overwrite, ancestry overlap, traversal and junction ambiguity are rejected', () => {
    const existing = fixture('existing'); fs.mkdirSync(existing.destination); put(path.join(existing.destination, 'keep.txt'), 'keep');
    assert.throws(() => prepare(existing), /destination_exists/);
    assert.equal(fs.readFileSync(path.join(existing.destination, 'keep.txt'), 'utf8'), 'keep');
    const overlap = fixture('overlap');
    assert.throws(() => prepare(overlap, { destinationRoot: path.join(overlap.source, 'copy') }), /source_destination_overlap/);
    assert.throws(() => prepare(overlap, { destinationRoot: overlap.base }), /source_destination_overlap/);
    const traversal = fixture('traversal'), header = path.join(traversal.source, iconHeader);
    put(header, fs.readFileSync(header, 'utf8').replace('icon-A.svg', '../../outside.svg'));
    failedCopy(traversal, /core_icon_filename/);
    const linked = fixture('junction'), target = path.join(linked.base, 'linked-target'); fs.mkdirSync(target);
    fs.symlinkSync(target, path.join(linked.source, 'Source/linked'), process.platform === 'win32' ? 'junction' : 'dir');
    failedCopy(linked, /symlink_or_junction/);
});

test('capture and preparation resolve explicit provider roots without cwd or environment fallback', () => {
    // Isolate the actual small capture function without executing its CLI.
    const code = fs.readFileSync(entrypoint, 'utf8').match(/^function configuredDependencyRoot\([\s\S]*?^\}/m)?.[0];
    assert(code);
    const captureRoot = vm.runInNewContext('(' + code + ')', { fs, path, requireThat: (ok, message) => { if (!ok) throw new Error(message); } });
    const source = fixture('provider').source, resolved = path.join(output, 'provider-resolved'), explicit = path.join(output, 'provider-explicit');
    fs.mkdirSync(resolved); fs.mkdirSync(explicit);
    for (const label of ['JUCE', 'ASIO_SDK']) {
        const keys = [`SINGLE_EDITOR_RESOLVED_${label}_ROOT`, `SINGLE_EDITOR_${label}_ROOT`];
        const select = text => [captureRoot(text, keys, source, label), resolveConfiguredDependencyRoot(text, keys, source, label).path];
        assert.deepEqual(select(`${keys[0]}:INTERNAL=${resolved}\n${keys[1]}:PATH=${explicit}`), [resolved, resolved]);
        assert.deepEqual(select(`${keys[0]}:INTERNAL=\n${keys[1]}:PATH=${explicit}`), [explicit, explicit]);
        assert.deepEqual(select(`${keys[1]}:PATH=${explicit}`), [explicit, explicit]);
        for (const rootFn of [text => captureRoot(text, keys, source, label), text => resolveConfiguredDependencyRoot(text, keys, source, label)]) {
            assert.throws(() => rootFn(`${keys[0]}:INTERNAL=\n${keys[1]}:PATH=`), /unresolved.dependency.root/);
            assert.throws(() => rootFn(`${keys[0]}:INTERNAL=${resolved}-missing\n${keys[1]}:PATH=${explicit}`), /missing.dependency.root/);
        }
    }
});

test('existing v1 compare still requires exact source root and HEAD', () => {
    const base = path.join(output, 'legacy-compare'); fs.mkdirSync(base);
    const paths = Array.from({ length: 4 }, (_, i) => path.join(base, `capture${i}.json`));
    const values = paths.map((_, i) => ({ schema: 'singleeditor.clean-build-capture', version: 1,
        phase: i % 2 ? 'built' : 'prepared', build_root: path.join(base, i < 2 ? 'a' : 'b'),
        source_root: 'retained-source', head: 'retained-head', inputs: { source: [] }, generator: {}, targets: [],
        binaries: [{ target: 'SingleEditor', path: path.join(base, i < 2 ? 'a/gui.exe' : 'b/gui.exe'), sha256: 'same' }] }));
    const save = () => values.forEach((v, i) => put(paths[i], JSON.stringify(v)));
    const compareArgs = suffix => ['compare', ...['--prepared-a', '--built-a', '--prepared-b', '--built-b'].flatMap((key, i) => [key, paths[i]]), '--output', path.join(base, suffix + '.json')];
    save(); assert.equal(cli(compareArgs('same')).status, 'pass');
    values[2].source_root = values[3].source_root = 'different-source'; save();
    assert(cli(compareArgs('different-source'), 1).differences.some(d => d.endsWith('source_root')));
    values[2].source_root = values[3].source_root = 'retained-source'; values[2].head = values[3].head = 'different-head'; save();
    assert(cli(compareArgs('different-head'), 1).differences.some(d => d.endsWith('head')));
});

const result = { schema: 'singleeditor.source-replay-focused-tests', version: 1, status: 'pass', cases,
    output, scope: 'Synthetic fixture source copies and retained v1 comparison behavior only. No real source replay, product configure, build or gate refresh.' };
fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify(result, null, 2) + '\n', { flag: 'wx' });
console.log(JSON.stringify(result));
