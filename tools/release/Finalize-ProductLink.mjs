// CMake post-link boundary. Archive immutable inputs/evidence before replacing
// only the fresh build output. Never operate on a packaged candidate.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { peFacts } from './BuildArtifactEvidence.mjs';

const [configuration, target, imageArgument, pdbArgument, inspectorArgument, diaArgument, rootArgument] = process.argv.slice(2);
const digest = b => crypto.createHash('sha256').update(b).digest('hex').toUpperCase();
const fact = p => ({ path: path.resolve(p), bytes: fs.statSync(p).size, sha256: digest(fs.readFileSync(p)) });
const read = p => JSON.parse(fs.readFileSync(p, 'utf8'));
const need = (ok, text) => { if (!ok) throw new Error(text); };
try {
    need(rootArgument && ['SingleEditor', 'SingleEditorCLI'].includes(target), 'Expected explicit CMake product link arguments.');
    if (configuration !== 'Release') {
        console.log('Private link finalization applies only to Release.');
    } else {
        const root = path.resolve(rootArgument), image = path.resolve(imageArgument), pdb = path.resolve(pdbArgument);
        const build = path.resolve(root, '../../..');
        const inside = (child, parent) => { const relative = path.relative(parent, child);
            return relative && !relative.startsWith('..' + path.sep) && relative !== '..' && !path.isAbsolute(relative); };
        need(root === path.join(build, 'link-evidence', target, 'Release') && fs.existsSync(path.join(build, 'CMakeCache.txt')),
            'Finalization requires the target private root of a configured build.');
        need(inside(image, build) && inside(pdb, root) && !inside(image, root), 'Only this build product and private PDB may be finalized.');
        const tools = path.dirname(fileURLToPath(import.meta.url));
        const toolInputs = [fileURLToPath(import.meta.url), path.join(tools, 'Finalize-LinkEvidence.mjs'),
            path.join(tools, 'BuildArtifactEvidence.mjs'), inspectorArgument, diaArgument].map(fact);
        const currentPath = path.join(root, 'current.json');
        const checkArchive = report => {
            need(report.schema === 'singleeditor.product-link-finalization' && report.version === 1
                && report.target === target, 'Unexpected product finalization record.');
            need(report.tools.length === toolInputs.length
                && report.tools.every((t, i) => t.sha256 === toolInputs[i].sha256), 'Relink after build-tool changes.');
            for (const tool of report.archived_tools)
                need(fact(tool.path).sha256 === tool.sha256, 'Archived build tool changed.');
            need(fact(report.finalization.path).sha256 === report.finalization.sha256, 'Archived finalization changed.');
            const finalization = read(report.finalization.path);
            for (const input of [...Object.values(finalization.inputs), finalization.output, finalization.contributions])
                need(fact(input.path).sha256 === input.sha256, 'Archived link input/evidence changed.');
            need(finalization.output.sha256 === report.final_image.sha256, 'Final image binding mismatch.');
            return finalization;
        };
        const before = fact(image), hasCodeView = peFacts(image).debug_types.includes(2);
        if (!hasCodeView) {
            need(fs.existsSync(currentPath), 'No raw CodeView link or current finalization record; relink required.');
            const current = read(currentPath); checkArchive(current);
            need(current.final_image.sha256 === before.sha256 && current.private_pdb.sha256 === fact(pdb).sha256,
                'Current finalized output/PDB mismatch; relink required.');
            console.log(JSON.stringify({ target, status: 'already-finalized', sha256: before.sha256 }));
        } else {
            const privatePdb = fact(pdb);
            const identity = digest(Buffer.from(JSON.stringify([before.sha256, privatePdb.sha256, ...toolInputs.map(t => t.sha256)])));
            const archive = path.join(root, 'history', identity), archiveRecord = path.join(archive, 'product.json');
            let report, finalization;
            if (fs.existsSync(archive)) {
                need(fs.existsSync(archiveRecord), 'Incomplete private link archive preserved; inspect it before retrying.');
                report = read(archiveRecord); finalization = checkArchive(report);
                need(report.linked_image.sha256 === before.sha256 && report.private_pdb.sha256 === privatePdb.sha256,
                    'Archive does not match the current raw link.');
            } else {
                fs.mkdirSync(path.join(archive, 'inputs'), { recursive: true });
                const archiveCopy = (input, name) => {
                    const destination = path.join(archive, 'inputs', name);
                    fs.copyFileSync(input, destination, fs.constants.COPYFILE_EXCL); return destination;
                };
                const rawImage = archiveCopy(image, path.basename(image)), rawPdb = archiveCopy(pdb, path.basename(pdb));
                const archiveInspector = archiveCopy(inspectorArgument, 'SingleEditorDiaInspector.exe');
                const archiveDia = archiveCopy(diaArgument, 'msdia140.dll');
                const finalizer = archiveCopy(path.join(tools, 'Finalize-LinkEvidence.mjs'), 'Finalize-LinkEvidence.mjs');
                const scanner = archiveCopy(path.join(tools, 'BuildArtifactEvidence.mjs'), 'BuildArtifactEvidence.mjs');
                const wrapper = archiveCopy(fileURLToPath(import.meta.url), 'Finalize-ProductLink.mjs');
                const finalRoot = path.join(archive, 'final');
                execFileSync(process.execPath, [finalizer, rawImage, rawPdb, archiveInspector, archiveDia, finalRoot],
                    { stdio: 'pipe', maxBuffer: 1024 * 1024 });
                finalization = read(path.join(finalRoot, 'receipt.json'));
                need(fact(rawImage).sha256 === before.sha256 && fact(rawPdb).sha256 === privatePdb.sha256,
                    'Link inputs changed while archiving.');
                report = { schema: 'singleeditor.product-link-finalization', version: 1, target, configuration,
                    status: 'build-finalized', release_eligible: false, tools: toolInputs,
                    archived_tools: [wrapper, finalizer, scanner, archiveInspector, archiveDia].map(fact),
                    linked_image: fact(rawImage), private_pdb: fact(rawPdb),
                    finalization: fact(path.join(finalRoot, 'receipt.json')),
                    final_image: { ...finalization.output, path: image } };
                fs.writeFileSync(archiveRecord, JSON.stringify(report, null, 2) + '\n', { flag: 'wx' });
                checkArchive(report);
            }
            // Publish recoverable evidence first. A repeated post-build command
            // can finish a raw link or validate the already-finalized image.
            need(fact(image).sha256 === before.sha256 && fact(pdb).sha256 === privatePdb.sha256,
                'Product link inputs changed before finalization.');
            for (const tool of toolInputs) need(fact(tool.path).sha256 === tool.sha256, 'Build tool changed before finalization.');
            const pointer = currentPath + '.new';
            fs.writeFileSync(pointer, JSON.stringify(report, null, 2) + '\n', { flag: 'wx' });
            fs.renameSync(pointer, currentPath);
            const temporary = image + '.finalizing';
            fs.copyFileSync(finalization.output.path, temporary, fs.constants.COPYFILE_EXCL);
            need(fact(temporary).sha256 === report.final_image.sha256, 'Final copy mismatch.');
            fs.renameSync(temporary, image);
            need(fact(image).sha256 === report.final_image.sha256, 'Published final file mismatch.');
            console.log(JSON.stringify({ target, status: report.status, sha256: report.final_image.sha256 }));
        }
    }
} catch (error) { console.error(error.message); process.exitCode = 1; }
