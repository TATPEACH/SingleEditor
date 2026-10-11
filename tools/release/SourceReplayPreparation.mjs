// Prepare an exact, bounded source replay copy. This neither creates a Git-clean
// checkout nor proves compiler/dependency completeness or binary reproducibility.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const need = (ok, message) => { if (!ok) throw new Error('source_replay_' + message); };
const slash = p => p.replaceAll('\\', '/');
const key = p => slash(path.resolve(p)).toLowerCase();
const hash = b => crypto.createHash('sha256').update(b).digest('hex').toUpperCase();
const fact = p => { const b = fs.readFileSync(p); return { path: path.resolve(p), bytes: b.length, sha256: hash(b) }; };
const compare = (a, b) => a < b ? -1 : a > b ? 1 : 0;
const manifestName = 'source-replay.json';
const projectEntries = ['CMakeLists.txt', 'Source', 'Tests', 'Resources', 'cmake'];
const iconContract = 'SingleEditorJuce/Source/UI/LookAndFeel/ModernNorthStarVisualContract.h';
const requiredReleaseTools = ['Finalize-ProductLink.mjs', 'Finalize-LinkEvidence.mjs', 'BuildArtifactEvidence.mjs',
    'dia/DiaSectionContributions.cpp', 'dia/CMakeLists.txt'];
const within = (root, target) => key(target) === key(root) || key(target).startsWith(key(root).replace(/\/$/, '') + '/');
const exists = p => { try { fs.lstatSync(p); return true; } catch (e) { if (e.code === 'ENOENT') return false; throw e; } };

function safeRelative(value) {
    need(typeof value === 'string' && value.length > 0 && !value.includes('\\') && !value.includes(':')
        && !value.includes('\0') && !path.isAbsolute(value) && !path.win32.isAbsolute(value)
        && value.split('/').every(p => p && p !== '.' && p !== '..'), 'invalid_relative_path:' + value);
    return value;
}
function noLinks(p, mustExist = true) {
    const absolute = path.resolve(p), chain = [];
    for (let next = absolute;; next = path.dirname(next)) {
        chain.push(next); if (path.dirname(next) === next) break;
    }
    for (const current of chain.reverse()) {
        if (!exists(current)) { need(!mustExist, 'missing_path:' + current); continue; }
        need(!fs.lstatSync(current).isSymbolicLink(), 'symlink_or_junction:' + current);
    }
    return absolute;
}
function selectedIcons(repository) {
    const file = path.join(repository, iconContract); noLinks(file);
    const text = fs.readFileSync(file, 'utf8'), rows = [...text.matchAll(
        /case ModernNorthStarAssetId::([A-Za-z]+): return \{ id, "([^"]+\.svg)", ModernNorthStarAssetOwner::core \}/g)];
    need(rows.length === 21, 'core_icon_membership_expected_21');
    const ids = new Set(), names = new Set();
    for (const [, id, name] of rows) {
        need(/^[A-Za-z0-9][A-Za-z0-9._-]*\.svg$/.test(name), 'core_icon_filename:' + name);
        need(!ids.has(id) && !names.has(name.toLowerCase()), 'duplicate_core_icon:' + name);
        ids.add(id); names.add(name.toLowerCase());
    }
    return rows.map(([, id, name]) => ({ id, path: 'icons/' + name })).sort((a, b) => compare(a.path, b.path));
}
function collect(source) {
    source = noLinks(source); need(fs.statSync(source).isDirectory(), 'source_not_directory');
    need(path.basename(source).toLowerCase() === 'singleeditorjuce', 'expected_singleeditorjuce_source_root');
    const repository = path.dirname(source), files = [], directories = new Set(['SingleEditorJuce', 'tools', 'icons']);
    const members = new Set();
    function add(relative, role) {
        safeRelative(relative); const absolute = path.join(repository, relative); noLinks(absolute);
        const stat = fs.lstatSync(absolute); need(!members.has(relative.toLowerCase()), 'overlapping_input:' + relative);
        members.add(relative.toLowerCase());
        if (stat.isDirectory()) {
            directories.add(relative);
            for (const name of fs.readdirSync(absolute).sort(compare)) {
                need(name !== '.git', 'git_metadata_in_input:' + relative);
                add(relative + '/' + name, role);
            }
        } else {
            need(stat.isFile(), 'non_regular_input:' + relative);
            const f = fact(absolute); files.push({ logical_path: relative, original_path: f.path, bytes: f.bytes, sha256: f.sha256, role });
        }
    }
    for (const entry of projectEntries) {
        const absolute = noLinks(path.join(source, entry)), stat = fs.statSync(absolute);
        need(entry === 'CMakeLists.txt' ? stat.isFile() : stat.isDirectory(), 'project_entry_type:' + entry);
        add('SingleEditorJuce/' + entry, 'project');
    }
    add('tools/release', 'release-tool');
    for (const file of requiredReleaseTools) need(files.some(f => f.logical_path === 'tools/release/' + file), 'missing_release_tool:' + file);
    const icons = selectedIcons(repository);
    for (const icon of icons) add(icon.path, 'declared-core-icon');
    files.sort((a, b) => compare(a.logical_path, b.logical_path));
    return { source_root: source, repository_root: repository, directories: [...directories].sort(compare), files, icons,
        icon_declaration: { logical_path: iconContract, ...fact(path.join(repository, iconContract)) } };
}
const contentRows = input => input.files.map(({ logical_path, bytes, sha256, role }) => ({ logical_path, bytes, sha256, role }));
const contentIdentity = input => hash(JSON.stringify({ directories: input.directories, files: contentRows(input) }));
function gitProvenance(source) {
    const run = args => spawnSync('git', args, { cwd: source, encoding: 'utf8', timeout: 10000,
        maxBuffer: 4 * 1024 * 1024, env: { ...process.env, GIT_OPTIONAL_LOCKS: '0' } });
    const root = run(['rev-parse', '--show-toplevel']);
    if (root.status !== 0) return { status: 'unavailable', head: null, dirty: null, reason: 'No readable Git repository; source content remains independently recorded.' };
    const head = run(['rev-parse', '--verify', 'HEAD']), status = run(['status', '--porcelain=v1', '--untracked-files=normal']);
    return { status: head.status === 0 && status.status === 0 ? 'observed' : 'partial', repository_root: root.stdout.trim(),
        head: head.status === 0 ? head.stdout.trim() : null, dirty: status.status === 0 ? status.stdout.length > 0 : null,
        porcelain_sha256: status.status === 0 ? hash(status.stdout) : null,
        meaning: 'Git provenance only; neither source content identity nor a claim that this replay copy is Git clean.' };
}
function cacheValues(text) {
    const values = new Map();
    for (const line of text.split(/\r?\n/)) {
        const match = line.match(/^([^#/:=][^:=]*):[^=]+=(.*)$/);
        if (match) values.set(match[1], match[2].trim());
    }
    return values;
}
export function resolveConfiguredDependencyRoot(cacheText, keys, sourceRoot, label) {
    const values = cacheValues(cacheText), selected = keys.find(k => values.get(k)?.trim());
    need(selected, 'unresolved_dependency_root:' + label + '; configure an explicit or resolved provider cache key');
    const value = values.get(selected);
    need(!/[;$\r\n]/.test(value) && !value.endsWith('-NOTFOUND'), 'unresolved_dependency_root:' + label);
    const absolute = path.isAbsolute(value) ? path.resolve(value) : path.resolve(sourceRoot, value);
    need(fs.existsSync(absolute) && fs.statSync(absolute).isDirectory(), 'missing_dependency_root:' + label + ':' + absolute);
    return { path: absolute, selected_key: selected, declared_value: value, resolved_path: fs.realpathSync.native(absolute) };
}
function dependencyBoundary(build, source) {
    const requirements = [
        { name: 'JUCE', keys: ['SINGLE_EDITOR_RESOLVED_JUCE_ROOT', 'SINGLE_EDITOR_JUCE_ROOT'] },
        { name: 'ASIO SDK', keys: ['SINGLE_EDITOR_RESOLVED_ASIO_SDK_ROOT', 'SINGLE_EDITOR_ASIO_SDK_ROOT'] },
        { name: 'libsamplerate', keys: ['libsamplerate_SOURCE_DIR'] }
    ];
    if (!build) return { status: 'not-selected', cache: null, roots: requirements.map(r => ({ ...r, status: 'required' })),
        content_verified: false, copied: false, complete: false,
        remaining: ['Select an explicit configured build cache; resolve and independently bind external dependencies, MSVC/Windows SDK, Node and DIA before building.'] };
    const cache = noLinks(path.join(path.resolve(build), 'CMakeCache.txt')), cacheFact = fact(cache), text = fs.readFileSync(cache, 'utf8');
    const values = cacheValues(text), home = values.get('CMAKE_HOME_DIRECTORY');
    need(home && key(home) === key(source), 'build_cache_source_mismatch');
    const roots = requirements.map(r => {
        try { return { name: r.name, ...resolveConfiguredDependencyRoot(text, r.keys, source, r.name), status: 'declared-existing-root', content_verified: false }; }
        catch (error) { return { ...r, status: 'unresolved', reason: error.message }; }
    });
    return { status: 'declared-only', cache: cacheFact, roots, content_verified: false, copied: false, complete: false,
        source_epoch: values.get('SINGLE_EDITOR_SOURCE_DATE_EPOCH') ?? null,
        link_evidence: values.get('SINGLE_EDITOR_ENABLE_LINK_EVIDENCE') ?? null,
        toolchain_cache: Object.fromEntries(['CMAKE_CXX_COMPILER', 'CMAKE_MAKE_PROGRAM', 'CMAKE_COMMAND',
            'CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION', 'SINGLE_EDITOR_NODE_EXECUTABLE', 'SINGLE_EDITOR_DIA_ROOT']
            .filter(k => values.has(k)).map(k => [k, values.get(k)])),
        remaining: ['Cache roots are declarations, not dependency byte inventories. Existing build-input witnesses must bind actual external reads/tools; no external dependency was copied or downloaded.'] };
}
function copyMembership(destination) {
    const files = [], directories = [];
    function visit(absolute, relative) {
        noLinks(absolute); const stat = fs.lstatSync(absolute);
        if (stat.isDirectory()) {
            if (relative) directories.push(relative);
            for (const name of fs.readdirSync(absolute).sort(compare)) {
                if (!relative && name === manifestName) continue;
                visit(path.join(absolute, name), relative ? relative + '/' + name : name);
            }
        } else { need(stat.isFile(), 'non_regular_copy:' + relative); files.push(relative); }
    }
    visit(destination, ''); return { files: files.sort(compare), directories: directories.sort(compare) };
}
function verifyCopy(destination, input) {
    const membership = copyMembership(destination);
    need(JSON.stringify(membership.files) === JSON.stringify(input.files.map(f => f.logical_path)), 'copied_file_membership_changed');
    need(JSON.stringify(membership.directories) === JSON.stringify(input.directories), 'copied_directory_membership_changed');
    for (const expected of input.files) {
        const actual = fact(path.join(destination, safeRelative(expected.logical_path)));
        need(actual.bytes === expected.bytes && actual.sha256 === expected.sha256, 'copied_bytes_changed:' + expected.logical_path);
    }
}
export function prepareSourceReplay({ sourceRoot, destinationRoot, buildRoot = null, entrypointPath = null }) {
    const source = noLinks(sourceRoot), repository = path.dirname(source), destination = noLinks(destinationRoot, false);
    need(!within(repository, destination) && !within(destination, repository), 'source_destination_overlap');
    need(!exists(destination), 'destination_exists:' + destination);
    let created = false, copied = 0;
    try {
        fs.mkdirSync(path.dirname(destination), { recursive: true }); noLinks(path.dirname(destination));
        fs.mkdirSync(destination); created = true;
        const beforeGit = gitProvenance(source), before = collect(source), dependencies = dependencyBoundary(buildRoot, source);
        const producers = [...new Set([fileURLToPath(import.meta.url), ...(entrypointPath ? [entrypointPath] : [])])].map(fact);
        for (const relative of before.directories) fs.mkdirSync(path.join(destination, relative), { recursive: true });
        for (const file of before.files) {
            const target = path.join(destination, file.logical_path); noLinks(file.original_path); noLinks(path.dirname(target));
            fs.copyFileSync(file.original_path, target, fs.constants.COPYFILE_EXCL); ++copied;
            const actual = fact(target); need(actual.bytes === file.bytes && actual.sha256 === file.sha256, 'copy_changed:' + file.logical_path);
        }
        const after = collect(source);
        need(JSON.stringify(before.directories) === JSON.stringify(after.directories)
            && JSON.stringify(contentRows(before)) === JSON.stringify(contentRows(after)), 'source_bytes_or_membership_changed');
        if (dependencies.cache) need(fact(dependencies.cache.path).sha256 === dependencies.cache.sha256, 'dependency_cache_changed');
        for (const producer of producers) need(fact(producer.path).sha256 === producer.sha256, 'producer_changed');
        verifyCopy(destination, before);
        const manifest = { schema: 'singleeditor.source-replay-copy', version: 1, status: 'prepared', prepared_utc: new Date().toISOString(),
            roots: { original_source: source, original_repository: repository, destination, copied_source: path.join(destination, 'SingleEditorJuce') },
            selection: { project_entries: projectEntries, release_tools: 'tools/release', icon_declaration: before.icon_declaration, icons: before.icons },
            directories: before.directories, files: before.files.map(f => ({ ...f, copied_path: path.join(destination, f.logical_path) })),
            content_identity_sha256: contentIdentity(before), git_provenance: { before: beforeGit, after: gitProvenance(source) }, producers,
            checks: { source_before_after_equal: true, copy_bytes_equal: true, copy_membership_equal: true },
            external_dependencies: dependencies, source_replay_verified: true, git_clean_checkout: false,
            complete_build_inputs: false, build_performed: false, binary_reproducibility_proven: false, release_eligible: false,
            scope: 'Current actual bytes in explicit project/release-tool roots and the 21 currently declared core icons, including untracked files. Source and copy membership/bytes are checked before/after. No Git metadata, old build outputs, external dependency trees or implicit SDK/header closure are copied. Git provenance is separate from content identity; this is not a clean-build or release pass.' };
        const manifestPath = path.join(destination, manifestName);
        fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n', { flag: 'wx' });
        return { status: 'prepared', manifest: fact(manifestPath), files: manifest.files.length,
            source_bytes: manifest.files.reduce((n, f) => n + f.bytes, 0), content_identity_sha256: manifest.content_identity_sha256,
            copied_source: manifest.roots.copied_source, external_dependency_status: dependencies.status,
            git_clean_checkout: false, complete_build_inputs: false, build_performed: false };
    } catch (error) {
        if (created) {
            try { fs.writeFileSync(path.join(destination, 'source-replay-failure.json'), JSON.stringify({ schema: 'singleeditor.source-replay-failure',
                version: 1, status: 'failed', source, destination, copied_files: copied, error: error.message,
                partial_copy_retained: true }, null, 2) + '\n', { flag: 'wx' }); } catch { /* Preserve the original error and all partial files. */ }
        }
        throw error;
    }
}
export function verifySourceReplay(manifestPath) {
    manifestPath = noLinks(manifestPath); const evidence = fact(manifestPath), manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    need(manifest.schema === 'singleeditor.source-replay-copy' && manifest.version === 1 && manifest.status === 'prepared'
        && manifest.git_clean_checkout === false && manifest.complete_build_inputs === false && manifest.build_performed === false
        && manifest.binary_reproducibility_proven === false && manifest.release_eligible === false, 'manifest_contract');
    const destination = noLinks(path.dirname(manifestPath));
    need(key(manifest.roots.destination) === key(destination) && path.basename(manifestPath) === manifestName, 'manifest_location');
    need(key(manifest.roots.original_source) === key(path.join(manifest.roots.original_repository, 'SingleEditorJuce'))
        && key(manifest.roots.copied_source) === key(path.join(destination, 'SingleEditorJuce')), 'manifest_source_binding');
    need(!within(manifest.roots.original_repository, destination) && !within(destination, manifest.roots.original_repository), 'manifest_root_overlap');
    const copy = collect(path.join(destination, 'SingleEditorJuce'));
    need(JSON.stringify(manifest.selection.project_entries) === JSON.stringify(projectEntries)
        && manifest.selection.release_tools === 'tools/release'
        && JSON.stringify(manifest.selection.icons) === JSON.stringify(copy.icons)
        && manifest.selection.icon_declaration.logical_path === iconContract
        && manifest.selection.icon_declaration.sha256 === copy.icon_declaration.sha256
        && manifest.selection.icon_declaration.bytes === copy.icon_declaration.bytes
        && key(manifest.selection.icon_declaration.path) === key(path.join(manifest.roots.original_repository, iconContract)), 'manifest_selection');
    for (const file of manifest.files) {
        const relative = safeRelative(file.logical_path);
        need(key(file.original_path) === key(path.join(manifest.roots.original_repository, relative))
            && key(file.copied_path) === key(path.join(destination, relative)), 'manifest_file_binding:' + relative);
    }
    need(JSON.stringify(contentRows(copy)) === JSON.stringify(contentRows(manifest))
        && JSON.stringify(copy.directories) === JSON.stringify(manifest.directories), 'copied_manifest_content_changed');
    need(contentIdentity(copy) === manifest.content_identity_sha256, 'content_identity_changed');
    verifyCopy(destination, manifest); need(fact(manifestPath).sha256 === evidence.sha256, 'manifest_changed');
    return { schema: 'singleeditor.source-replay-verification', version: 1, status: 'pass', manifest: evidence,
        files: manifest.files.length, content_identity_sha256: manifest.content_identity_sha256,
        git_clean_checkout: false, complete_build_inputs: false, binary_reproducibility_proven: false,
        scope: 'Read-only verification of the retained copy and its declared membership. It does not relabel the original repository or prove a build.' };
}
