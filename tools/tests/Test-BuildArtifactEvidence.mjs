import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { observeBuild, reconcileSources, validateTestObserverIsolation } from '../release/BuildArtifactEvidence.mjs';

const [capturePath, observationPath, output, peerObservationPath] = process.argv.slice(2);
if (!capturePath || !observationPath || !output) throw new Error('Usage: <built-capture> <observation.json> <new-output-directory>');
if (fs.existsSync(output)) throw new Error('Output already exists.');
fs.mkdirSync(output, { recursive: true });
const read = p => JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
const hash = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex').toUpperCase();
const observed = read(observationPath), capture = read(capturePath);
const compiler = capture.inputs.toolchain.find(t => t.path.endsWith('/cl.exe'));
const linker = path.join(path.dirname(compiler.path), 'link.exe');
const results = [];
function check(name, action) {
    try { action(); results.push({ name, status: 'pass' }); }
    catch (error) {
        fs.writeFileSync(path.join(output,'failure.json'),JSON.stringify({name,message:error.message,stack:error.stack,
            actual:error.actual,expected:error.expected},null,2)+'\n',{flag:'wx'});
        throw new Error(`${name}: failed; see ${path.join(output,'failure.json')}`);
    }
}
const sort = values => [...values].sort((a,b) => JSON.stringify(a).localeCompare(JSON.stringify(b)));
function checkFinalizationBindings(observation) {
    for (const product of observation.products.filter(p => p.link_finalization)) {
        const binding = product.link_finalization;
        const full = p => p.replace('<BUILD>', observation.roots.build).replace('<SOURCE>', observation.roots.source);
        for (const item of [binding.record, binding.finalization, binding.contributions])
            assert.equal(hash(full(item.path)), item.sha256);
        const record = read(full(binding.record.path)), finalization = read(full(binding.finalization.path));
        assert.equal(hash(record.final_image.path), product.sha256);
        assert.equal(finalization.output.sha256, product.sha256);
        assert.equal(finalization.inputs.image.sha256, record.linked_image.sha256);
        assert.equal(finalization.inputs.pdb.sha256, record.private_pdb.sha256);
        assert.deepEqual(finalization.pdb_identity, binding.pdb_identity);
        for (const item of [...record.archived_tools, ...Object.values(finalization.inputs),
            finalization.output, finalization.contributions]) assert.equal(hash(item.path), item.sha256);
        assert.equal(product.debug_types.includes(2), false);
        assert.equal(product.debug_types.includes(12), false);
    }
}
if (capture.inputs.link_evidence) {
    check('actual GUI and CLI finalization bindings', () => {
        assert.equal(observed.products.filter(p => p.link_finalization).length, 2);
        checkFinalizationBindings(observed);
    });
    for (const product of observed.products) check(product.target + ': repeated post-build preserves final bytes', () => {
        const pointer = path.join(observed.roots.build, 'link-evidence', product.target, 'Release', 'current.json');
        const record = read(pointer), before = hash(pointer);
        const result = JSON.parse(execFileSync(process.execPath, [path.resolve('tools/release/Finalize-ProductLink.mjs'),
            'Release', product.target, record.final_image.path,
            path.join(path.dirname(pointer), product.target + '.pdb'), record.tools[3].path, record.tools[4].path,
            path.dirname(pointer)], { encoding: 'utf8' }));
        assert.equal(result.status, 'already-finalized');
        assert.equal(hash(pointer), before);
        assert.equal(hash(record.final_image.path), product.sha256);
    });
}
for (const product of observed.products) {
    const exe = path.join(observed.roots.payload, product.payload_path);
    const dump = flag => {
        const text = execFileSync(linker, ['/dump', flag, exe], { encoding:'utf8', maxBuffer: 16 * 1024 * 1024 });
        fs.writeFileSync(path.join(output, product.target + '-' + flag.slice(1) + '.txt'), text, { flag:'wx' }); return text;
    };
    const headers = dump('/headers'), imports = dump('/imports');
    check(product.target + ': native PE headers', () => {
        assert.equal(hash(exe),product.sha256);
        assert.equal(parseInt(headers.match(/^\s+([0-9A-F]+) number of sections\s*$/m)[1],16), product.sections.length);
        assert.equal(parseInt(headers.match(/^\s+([0-9A-F]+) size of image\s*$/m)[1],16), product.image_bytes);
        const blocks = headers.split(/SECTION HEADER #\d+/).slice(1);
        const actual = blocks.map(block => ({
            name: block.match(/^\s+(\S+) name\s*$/m)[1],
            virtual_bytes: parseInt(block.match(/^\s+([0-9A-F]+) virtual size\s*$/m)[1],16),
            raw_bytes: parseInt(block.match(/^\s+([0-9A-F]+) size of raw data\s*$/m)[1],16),
        }));
        assert.deepEqual(actual, product.sections.map(({name,virtual_bytes,raw_bytes})=>({name,virtual_bytes,raw_bytes})));
    });
    check(product.target + ': native import symbols', () => {
        const rows=[];let current;
        for(const line of imports.split(/\r?\n/)) {
            if (/^\s*Summary\s*$/.test(line)) break;
            const dll=line.match(/^\s+([A-Za-z0-9_.-]+\.dll)\s*$/);
            if(dll) { current={dll:dll[1].toLowerCase(),symbols:[]}; rows.push(current); continue; }
            const symbol=line.match(/^\s+([0-9A-F]+) (\S+)\s*$/);
            if(current && symbol) current.symbols.push({name:symbol[2],hint:parseInt(symbol[1],16)});
            const ordinal=line.match(/^\s+Ordinal\s+(\d+)\s*$/);
            if(current && ordinal) current.symbols.push({ordinal:Number(ordinal[1])});
        }
        assert.deepEqual(rows, product.imports);
    });
    check(product.target + ': Windows resource bytes and version', () => {
        const raw=execFileSync('powershell.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',
            path.resolve('tools/tests/Read-NativeProductResources.ps1'),'-Executable',exe],{encoding:'utf8',maxBuffer:2*1024*1024});
        fs.writeFileSync(path.join(output,product.target+'-native-resources.json'),raw,{flag:'wx'});
        const native=JSON.parse(raw.replace(/^\uFEFF/,''));
        assert.deepEqual(sort(native.resources),sort(product.resources.map(({ids,bytes,sha256})=>({ids,bytes,sha256}))));
        assert.equal(native.product_version,'0.1.0');
    });
}
const sources = observed.targets.find(t=>t.name==='SingleEditor').compiled.map(t=>t.source);
const observerMacro = 'SINGLE_EDITOR_ENABLE_TEST_OBSERVERS';
const observerGroups = [{ defines: [{ define: observerMacro + '=0' }] }];
const observerCommands = [{ values: ['/D ' + observerMacro + '=0'] }];
check('reviewed observer boundary from actual product compilation', () => {
    for (const name of ['SingleEditor', 'SingleEditorCLI']) {
        const boundary = observed.targets.find(t => t.name === name).test_observer_isolation;
        assert.equal(boundary.value, 0);
        assert.ok(boundary.configured_groups > 0 && boundary.observed_commands > 0);
    }
});
check('observer compile boundary accepts explicit product exclusion', () =>
    validateTestObserverIsolation('SingleEditor', observerGroups, observerCommands));
check('observer compile boundary rejects configured test enablement', () => assert.throws(() =>
    validateTestObserverIsolation('SingleEditor', [{defines:[{define:observerMacro+'=1'}]}], observerCommands), /test_observers_not_isolated/));
check('observer compile boundary rejects actual compiler override', () => assert.throws(() =>
    validateTestObserverIsolation('SingleEditor', observerGroups, [{values:['/D '+observerMacro+'=0 /D "'+observerMacro+'=1"']}]), /test_observers_not_isolated/));
check('observer compile boundary rejects an undefined shipping boundary', () => assert.throws(() =>
    validateTestObserverIsolation('SingleEditorCLI', observerGroups, [{values:['/D '+observerMacro+'=0 /U '+observerMacro]}]), /test_observers_not_isolated/));
check('observer compile boundary rejects missing shipping evidence', () => assert.throws(() =>
    validateTestObserverIsolation('SingleEditor', [], []), /test_observers_not_isolated/));
check('linked libraries cannot enable reviewed observers', () => assert.throws(() =>
    validateTestObserverIsolation('support', [], [{values:['/D '+observerMacro+'=1']}]), /test_observers_not_isolated/));
check('unrelated library has no artificial product requirement', () =>
    validateTestObserverIsolation('support', [{defines:[]}], [{values:['/D SUPPORT_LIBRARY=1']}]));
check('exact compilation set',()=>reconcileSources(sources,sources));
check('missing compiled source rejected',()=>assert.throws(()=>reconcileSources(sources,sources.slice(1)),/declared_source_missing/));
check('extra compiled source rejected',()=>assert.throws(()=>reconcileSources(sources,[...sources,'unexpected.cpp']),/observed_source_undeclared/));
check('duplicate compiled source rejected',()=>assert.throws(()=>reconcileSources(sources,[...sources,sources[0]]),/duplicate_observed_source/));
check('duplicate declared source rejected',()=>assert.throws(()=>reconcileSources([...sources,sources[0]],sources),/duplicate_declared_source/));
check('input directory refuses output',()=>assert.throws(()=>observeBuild(capturePath,observed.roots.payload,
    path.join(observed.roots.payload,'unused-build-evidence')),/output_inside_input/));
const changed=structuredClone(capture); changed.inputs.source[0].sha256='0'.repeat(64);
const changedPath=path.join(output,'changed-source-capture.json');fs.writeFileSync(changedPath,JSON.stringify(changed),{flag:'wx'});
check('changed captured source rejected',()=>assert.throws(()=>observeBuild(changedPath,observed.roots.payload,path.join(output,'rejected-source')),/source_changed_since_build/));
const wrong=structuredClone(capture);wrong.binaries[0].sha256='0'.repeat(64);
const wrongPath=path.join(output,'wrong-product-capture.json');fs.writeFileSync(wrongPath,JSON.stringify(wrong),{flag:'wx'});
check('wrong product hash rejected',()=>assert.throws(()=>observeBuild(wrongPath,observed.roots.payload,path.join(output,'rejected-product')),/product_hash_mismatch/));
check('refuses existing observation output',()=>assert.throws(()=>observeBuild(capturePath,observed.roots.payload,output),/output_exists/));
assert.equal(observed.release_eligible,false);
if (peerObservationPath) check('independent final product facts and variant delta',()=>{
    const peer=read(peerObservationPath);
    assert.notEqual(observed.roots.build,peer.roots.build);
    for(const p of peer.products) assert.equal(hash(path.join(peer.roots.payload,p.payload_path)),p.sha256);
    checkFinalizationBindings(peer);
    const facts=v=>v.products.map(({link_command,link_read,observed_link_inputs,link_finalization,...p})=>({ ...p,
        ...(link_finalization ? { finalization_identity: { status: link_finalization.status,
            pdb_identity: link_finalization.pdb_identity, final_image_sha256: link_finalization.final_image_sha256,
            owner_status: link_finalization.owner_status } } : {}) }));
    assert.deepEqual(facts(observed),facts(peer));
    assert.deepEqual(observed.variant_delta,peer.variant_delta);
});
const receipt={schema:'singleeditor.build-artifact-evidence-tests',version:1,status:'pass',
    inputs:[capturePath,observationPath,...(peerObservationPath?[peerObservationPath]:[])].map(p=>({path:path.resolve(p),sha256:hash(p)})),
    tools:[new URL(import.meta.url),new URL('../release/BuildArtifactEvidence.mjs',import.meta.url),
        new URL('./Read-NativeProductResources.ps1',import.meta.url)].map(p=>({path:p.pathname,sha256:hash(p)})),results};
fs.writeFileSync(path.join(output,'receipt.json'),JSON.stringify(receipt,null,2)+'\n',{flag:'wx'});
console.log(JSON.stringify(receipt));
