#!/usr/bin/env python3
"""Read-only recovery of FAT12/16/32 files from an ESP-IDF WL flash backup.

Run with the ESP-IDF Python environment (construct dependency). The original
image is never modified. Extracted audio and manifests belong outside Git.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys


class FatReader:
    def __init__(self, data: bytes):
        self.data = data
        self.sector = self.u16(11)
        self.spc = data[13]
        self.reserved = self.u16(14)
        self.fats = data[16]
        self.root_entries = self.u16(17)
        self.sectors = self.u16(19) or self.u32(32)
        self.fat_sectors = self.u16(22) or self.u32(36)
        if self.sector not in (512, 1024, 2048, 4096) or not self.spc or self.spc & (self.spc-1):
            raise ValueError("invalid FAT sector/cluster geometry")
        if data[510:512] != b"\x55\xaa":
            raise ValueError("invalid FAT boot signature")
        self.root_sectors = (self.root_entries*32+self.sector-1)//self.sector
        self.root_offset = (self.reserved+self.fats*self.fat_sectors)*self.sector
        self.data_offset = self.root_offset+self.root_sectors*self.sector
        self.clusters = (self.sectors*self.sector-self.data_offset)//(self.sector*self.spc)
        self.bits = 12 if self.clusters < 4085 else 16 if self.clusters < 65525 else 32
        self.fat_offset = self.reserved*self.sector
        if self.sectors*self.sector > len(data):
            raise ValueError("filesystem extends beyond available image")

    def u16(self, at):
        return struct.unpack_from("<H", self.data, at)[0]

    def u32(self, at):
        return struct.unpack_from("<I", self.data, at)[0]

    def next_cluster(self, cluster):
        if self.bits == 12:
            value=self.u16(self.fat_offset+cluster+cluster//2)
            return value >> 4 if cluster & 1 else value & 0xfff
        if self.bits == 16:
            return self.u16(self.fat_offset+cluster*2)
        return self.u32(self.fat_offset+cluster*4) & 0xfffffff

    def chain(self, cluster, size=None):
        if size == 0:
            return b""
        out=bytearray()
        seen=set()
        end=0xff8 if self.bits == 12 else 0xfff8 if self.bits == 16 else 0xffffff8
        while cluster < end:
            if cluster < 2 or cluster >= self.clusters+2 or cluster in seen:
                raise ValueError("invalid or cyclic cluster chain")
            seen.add(cluster)
            at=self.data_offset+(cluster-2)*self.sector*self.spc
            out.extend(self.data[at:at+self.sector*self.spc])
            if size is not None and len(out) >= size:
                return bytes(out[:size])
            cluster=self.next_cluster(cluster)
        if size is not None and len(out) < size:
            raise ValueError("truncated cluster chain")
        return bytes(out if size is None else out[:size])

    def root(self):
        if self.bits == 32:
            return self.chain(self.u32(44))
        return self.data[self.root_offset:self.root_offset+self.root_entries*32]

    def entries(self, directory):
        long_parts={}
        for at in range(0,len(directory),32):
            entry=directory[at:at+32]
            if len(entry)<32 or entry[0] == 0:
                break
            if entry[0] == 0xe5:
                long_parts={}; continue
            if entry[11] == 0x0f:
                if entry[0]&0x40:
                    long_parts={}
                long_parts[entry[0]&0x1f]=(entry[1:11]+entry[14:26]+entry[28:32]).decode("utf-16le","replace")
                continue
            short=entry[:8].decode("cp437").rstrip()
            ext=entry[8:11].decode("cp437").rstrip()
            name="".join(long_parts[i] for i in sorted(long_parts)).split("\0",1)[0].rstrip("\uffff") if long_parts else short+("."+ext if ext else "")
            long_parts={}
            if entry[11]&8 or name in (".",".."):
                continue
            if not name or Path(name).name != name or "\\" in name:
                raise ValueError("unsafe recovered file name")
            cluster=struct.unpack_from("<H",entry,26)[0]
            if self.bits == 32:
                cluster|=struct.unpack_from("<H",entry,20)[0]<<16
            yield name,entry[11],cluster,struct.unpack_from("<I",entry,28)[0]


def recover(image: Path, output: Path, *, offset: int, length: int, idf_path: Path):
    if output.exists():
        raise ValueError("output must be a new directory")
    source=image.read_bytes()
    if offset+length>len(source):
        raise ValueError("partition exceeds backup")
    sys.path.insert(0,str(idf_path/"components/fatfs"))
    from wl_fatfsgen import remove_wl
    plain=remove_wl(source[offset:offset+length])
    fat=FatReader(plain)
    output.mkdir(parents=True,mode=0o700)
    os.chmod(output,0o700)
    (output/"filesystem-readonly.img").write_bytes(plain)
    manifest=[]
    errors=[]
    def extract(directory, relative=Path(".")):
        for name,attr,cluster,size in fat.entries(directory):
            rel=relative/name
            target=output/"files"/rel
            if attr&0x10:
                target.mkdir(parents=True,exist_ok=True,mode=0o700)
                extract(fat.chain(cluster),rel)
                continue
            try:
                data=fat.chain(cluster,size)
                target.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
                with target.open("xb") as stream:
                    stream.write(data)
                target.chmod(0o600)
                manifest.append({"file":str(rel),"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"cluster":cluster})
            except (OSError,ValueError) as exc:
                errors.append({"file":str(rel),"error":str(exc)})
    root=fat.root()
    extract(root)
    active=sum(1 for i in range(0,len(root),32) if root[i] not in (0,0xe5))
    free_clusters=sum(fat.next_cluster(c)==0 for c in range(2,fat.clusters+2))
    result={"backup_sha256":hashlib.sha256(source).hexdigest(),"fat_bits":fat.bits,
            "sector_bytes":fat.sector,"sectors_per_cluster":fat.spc,"root_entry_limit":fat.root_entries,
            "root_active_entries":active,"free_cluster_bytes":free_clusters*fat.sector*fat.spc,
            "files":manifest,"errors":errors}
    (output/"manifest.json").write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
    for file in output.iterdir():
        if file.is_file():file.chmod(0o600)
    return result


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image",type=Path)
    parser.add_argument("output",type=Path)
    parser.add_argument("--offset",type=lambda x:int(x,0),default=0x610000)
    parser.add_argument("--length",type=lambda x:int(x,0),default=0x19f0000)
    parser.add_argument("--idf-path",type=Path,default=Path(os.environ.get("IDF_PATH",Path.home()/"esp/esp-idf")))
    args=parser.parse_args()
    result=recover(args.image,args.output,offset=args.offset,length=args.length,idf_path=args.idf_path)
    print(json.dumps({**{k:v for k,v in result.items() if k not in ("files","errors")},"recovered_files":len(result["files"]),"errors":result["errors"]},ensure_ascii=False,indent=2))
