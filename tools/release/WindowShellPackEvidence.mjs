import { crc32 } from 'node:zlib';

// Reader for the declared two-entry stored ZIP produced by the window-shell
// pack build tool. No extraction or application execution.
export function readOwnerDataPack(bytes, dataEntryName, maximumBytes, errorPrefix) {
    const requireFact=(ok,code)=>{if(!ok)throw new Error(`${errorPrefix}_pack_${code}`);};
    requireFact(bytes.length>=22 && bytes.length<=maximumBytes,'size');
    const end=bytes.length-22;
    requireFact(bytes.readUInt32LE(end)===0x06054b50 && bytes.readUInt16LE(end+20)===0
        && bytes.readUInt32LE(end+4)===0 && bytes.readUInt16LE(end+8)===2
        && bytes.readUInt16LE(end+10)===2,'directory');
    let p=bytes.readUInt32LE(end+16),local=0;
    requireFact(p+bytes.readUInt32LE(end+12)===end,'directory_bounds');
    const entries=[];
    for(const name of ['manifest.json',dataEntryName]) {
        requireFact(p+46<=end && bytes.readUInt32LE(p)===0x02014b50,'entry');
        const size=bytes.readUInt32LE(p+24),nameBytes=bytes.readUInt16LE(p+28),offset=bytes.readUInt32LE(p+42);
        requireFact(p+46+nameBytes<=end && bytes.subarray(p+46,p+46+nameBytes).toString('utf8')===name
            && bytes.readUInt16LE(p+8)===0 && bytes.readUInt16LE(p+10)===0
            && bytes.readUInt32LE(p+20)===size && bytes.readUInt16LE(p+30)===0
            && bytes.readUInt16LE(p+32)===0 && offset===local,'entry_contract');
        requireFact(offset+30+nameBytes+size<=bytes.readUInt32LE(end+16)
            && bytes.readUInt32LE(offset)===0x04034b50 && bytes.readUInt16LE(offset+6)===0
            && bytes.readUInt16LE(offset+8)===0 && bytes.readUInt16LE(offset+28)===0
            && bytes.readUInt16LE(offset+26)===nameBytes
            && bytes.readUInt32LE(offset+18)===size && bytes.readUInt32LE(offset+22)===size
            && bytes.subarray(offset+30,offset+30+nameBytes).toString('utf8')===name,'local_entry');
        const dataOffset=offset+30+nameBytes,data=bytes.subarray(dataOffset,dataOffset+size);
        requireFact(crc32(data)===bytes.readUInt32LE(p+16) && crc32(data)===bytes.readUInt32LE(offset+14),'entry_crc');
        entries.push({name,bytes:size,compressed_bytes:size,file_offset:dataOffset,data});
        local=dataOffset+size;p+=46+nameBytes;
    }
    requireFact(p===end && local===bytes.readUInt32LE(end+16),'unexpected_content');
    return {entries,manifest:JSON.parse(entries[0].data.toString('utf8'))};
}

export function readWindowShellPack(bytes) {
    return readOwnerDataPack(bytes,'tray-icon.png',512*1024,'tray');
}
