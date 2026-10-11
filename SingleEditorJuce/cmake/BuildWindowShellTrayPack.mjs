// Build-only deterministic two-entry owner ZIP. The default preserves the
// window-shell pack byte contract; Ambisonics passes its explicit data entry.
// No timestamps, host attributes, directory entries or extra fields.
import fs from 'node:fs';
import { crc32 } from 'node:zlib';
const [manifestPath,imagePath,output,entryName='tray-icon.png']=process.argv.slice(2);
if(!manifestPath || !imagePath || !output || !['tray-icon.png','binaural.sehrir'].includes(entryName))
    throw new Error('Expected manifest, owner data, build-output path and a known entry name');
const entries=[['manifest.json',fs.readFileSync(manifestPath)],[entryName,fs.readFileSync(imagePath)]];
const local=[],central=[];let offset=0;
for(const [name,bytes] of entries) {
    const filename=Buffer.from(name),crc=crc32(bytes),header=Buffer.alloc(30),directory=Buffer.alloc(46);
    header.writeUInt32LE(0x04034b50,0);header.writeUInt16LE(20,4);
    header.writeUInt16LE(33,12); // DOS 1980-01-01, midnight.
    header.writeUInt32LE(crc,14);header.writeUInt32LE(bytes.length,18);header.writeUInt32LE(bytes.length,22);
    header.writeUInt16LE(filename.length,26);
    directory.writeUInt32LE(0x02014b50,0);directory.writeUInt16LE(20,4);directory.writeUInt16LE(20,6);
    directory.writeUInt16LE(33,14);directory.writeUInt32LE(crc,16);
    directory.writeUInt32LE(bytes.length,20);directory.writeUInt32LE(bytes.length,24);
    directory.writeUInt16LE(filename.length,28);directory.writeUInt32LE(offset,42);
    local.push(header,filename,bytes);central.push(directory,filename);offset+=header.length+filename.length+bytes.length;
}
const directory=Buffer.concat(central),end=Buffer.alloc(22);
end.writeUInt32LE(0x06054b50,0);end.writeUInt16LE(entries.length,8);end.writeUInt16LE(entries.length,10);
end.writeUInt32LE(directory.length,12);end.writeUInt32LE(offset,16);
const pack=Buffer.concat([...local,directory,end]);
if(!fs.existsSync(output) || !fs.readFileSync(output).equals(pack)) fs.writeFileSync(output,pack);
