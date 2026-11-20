#!/usr/bin/env python3
"""Exercise actual IDF FatFs on an in-memory COPY of a recovered FAT image.

Never writes the input image or touches a device. Reproduces long-name rename
failure in a fixed FAT12 root, then verifies a growable rec directory and 360
20-second recordings. Requires clang and an ESP-IDF source tree.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

CONFIG = '''
#define FFCONF_DEF 80386
#define FF_FS_READONLY 0
#define FF_FS_MINIMIZE 0
#define FF_USE_FIND 0
#define FF_USE_MKFS 0
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND 0
#define FF_USE_CHMOD 0
#define FF_USE_LABEL 0
#define FF_USE_FORWARD 0
#define FF_USE_STRFUNC 0
#define FF_PRINT_LLI 0
#define FF_PRINT_FLOAT 0
#define FF_STRF_ENCODE 3
#define FF_CODE_PAGE 437
#define FF_USE_LFN 2
#define FF_MAX_LFN 255
#define FF_LFN_UNICODE 0
#define FF_LFN_BUF 255
#define FF_SFN_BUF 12
#define FF_FS_RPATH 0
#define FF_PATH_DEPTH 10
#define FF_VOLUMES 1
#define FF_STR_VOLUME_ID 0
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS 4096
#define FF_MAX_SS 4096
#define FF_LBA64 0
#define FF_MIN_GPT 0x10000000
#define FF_USE_TRIM 0
#define FF_FS_TINY 0
#define FF_FS_EXFAT 0
#define FF_FS_NORTC 1
#define FF_NORTC_MON 1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026
#define FF_FS_CRTIME 0
#define FF_FS_NOFSINFO 0
#define FF_FS_LOCK 0
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT 1000
#define FF_USE_DYN_BUFFER 0
'''

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
static unsigned char *disk, *original; static size_t bytes;
DSTATUS disk_initialize(BYTE d) { return d?STA_NOINIT:0; }
DSTATUS disk_status(BYTE d) { return d?STA_NOINIT:0; }
DRESULT disk_read(BYTE d,BYTE *b,LBA_t s,UINT n) {
    if(d || (size_t)s*4096+(size_t)n*4096>bytes) return RES_PARERR;
    memcpy(b,disk+(size_t)s*4096,(size_t)n*4096); return RES_OK;
}
DRESULT disk_write(BYTE d,const BYTE *b,LBA_t s,UINT n) {
    if(d || (size_t)s*4096+(size_t)n*4096>bytes) return RES_PARERR;
    memcpy(disk+(size_t)s*4096,b,(size_t)n*4096); return RES_OK;
}
DRESULT disk_ioctl(BYTE d,BYTE cmd,void *b) {
    if(cmd==CTRL_SYNC) return RES_OK;
    if(cmd==GET_SECTOR_COUNT) { *(LBA_t *)b=bytes/4096; return RES_OK; }
    if(cmd==GET_SECTOR_SIZE) { *(WORD *)b=4096; return RES_OK; }
    if(cmd==GET_BLOCK_SIZE) { *(DWORD *)b=1; return RES_OK; }
    return RES_PARERR;
}
static void mount_copy(FATFS *fs) {
    f_mount(NULL,"0:",0); memcpy(disk,original,bytes);
    assert(f_mount(fs,"0:",1)==FR_OK);
    assert(fs->fs_type==FS_FAT12 && fs->n_rootdir==512);
}
static void write_segment(const char *part,const char *final,int full) {
    FIL f; UINT written; static BYTE data[51717];
    memset(data,0x5a,sizeof(data));
    assert(f_open(&f,part,FA_WRITE|FA_CREATE_NEW)==FR_OK);
    UINT size=full?sizeof(data):1;
    assert(f_write(&f,data,size,&written)==FR_OK && written==size);
    assert(f_sync(&f)==FR_OK && f_close(&f)==FR_OK);
    assert(f_rename(part,final)==FR_OK);
    assert(f_open(&f,final,FA_READ)==FR_OK);
    BYTE readback[51717]; UINT n;
    assert(f_read(&f,readback,size,&n)==FR_OK && n==size && !memcmp(data,readback,size));
    assert(f_close(&f)==FR_OK);
}
int main(int argc,char **argv) {
    assert(argc==2); FILE *input=fopen(argv[1],"rb"); assert(input);
    fseek(input,0,SEEK_END); bytes=ftell(input); rewind(input);
    original=malloc(bytes); disk=malloc(bytes); assert(original && disk);
    assert(fread(original,1,bytes,input)==bytes); fclose(input);
    FATFS fs; mount_copy(&fs); DWORD free_clusters; FATFS *ptr;
    assert(f_getfree("0:",&free_clusters,&ptr)==FR_OK);
    unsigned long free_bytes=(unsigned long)free_clusters*fs.csize*4096;
    /* Existing complete .part needs four new LFN/SFN entries. */
    FRESULT rename_rc=f_rename("0:/q_0000995610_1795147944.opus.part",
                               "0:/q_0000995610_1795147944.opus");
    FIL f;
    FRESULT open_rc=f_open(&f,"0:/q_0000995611_1795147964.opus.part",FA_WRITE|FA_CREATE_NEW);
    if(open_rc==FR_OK) f_close(&f);
    assert(rename_rc!=FR_OK || open_rc!=FR_OK);
    printf("{\"root_rename_result\":%d,\"root_next_open_result\":%d,\"free_bytes\":%lu,",rename_rc,open_rc,free_bytes);
    mount_copy(&fs);
    assert(f_mkdir("0:/rec")==FR_OK);
    /* Move a complete interrupted file; all existing audio bytes are unchanged. */
    assert(f_rename("0:/q_0000995610_1795147944.opus.part",
                    "0:/rec/q_0000995610_1795147944.opus")==FR_OK);
    for(int i=0;i<200;i++) {
        char part[100],final[100];
        snprintf(final,sizeof(final),"0:/rec/q_%010d_%010d.opus",1000000+i,1795200000+i*20);
        snprintf(part,sizeof(part),"%s.part",final); write_segment(part,final,0);
    }
    printf("\"subdir_long_names_verified\":200,");
    /* Separate clean-volume capacity scenario, only deletes from RAM copy. */
    mount_copy(&fs);
    FF_DIR dir; FILINFO info; char names[200][260]; int count=0;
    assert(f_opendir(&dir,"0:/")==FR_OK);
    while(f_readdir(&dir,&info)==FR_OK && info.fname[0]) {
        assert(!(info.fattrib&AM_DIR)); assert(count<200);
        snprintf(names[count++],260,"0:/%s",info.fname);
    }
    f_closedir(&dir);
    for(int i=0;i<count;i++) assert(f_unlink(names[i])==FR_OK);
    assert(f_mkdir("0:/rec")==FR_OK);
    for(int i=0;i<360;i++) {
        char part[100],final[100];
        snprintf(final,sizeof(final),"0:/rec/q_%010d_%010d.opus",2000000+i,1795200000+i*20);
        snprintf(part,sizeof(part),"%s.part",final); write_segment(part,final,1);
    }
    assert(f_getfree("0:",&free_clusters,&ptr)==FR_OK);
    unsigned long remaining=(unsigned long)free_clusters*fs.csize*4096;
    assert(remaining>1024*1024);
    printf("\"two_hour_segments_written_and_read\":360,\"remaining_bytes\":%lu}\n",remaining);
    f_mount(NULL,"0:",0); free(disk); free(original);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('image', type=Path)
    p.add_argument('--idf-path', type=Path, default=Path(os.environ.get('IDF_PATH', Path.home()/'esp/esp-idf')))
    args = p.parse_args()
    before = hashlib.sha256(args.image.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='fat-queue-test-') as td:
        tmp = Path(td)
        for name in ('ff.c', 'ff.h', 'diskio.h', 'ffunicode.c'):
            shutil.copyfile(args.idf_path/'components/fatfs/src'/name, tmp/name)
        (tmp/'ffconf.h').write_text(CONFIG)
        (tmp/'test.c').write_text(HARNESS)
        subprocess.run(['clang', '-g', '-O1', '-fsanitize=address,undefined', '-include', 'stddef.h', '-I', str(tmp),
                        str(tmp/'ff.c'), str(tmp/'ffunicode.c'), str(tmp/'test.c'), '-o', str(tmp/'test')], check=True)
        result = subprocess.run([str(tmp/'test'), str(args.image)], capture_output=True, text=True, check=True)
        print(result.stdout.strip())
    assert before == hashlib.sha256(args.image.read_bytes()).hexdigest(), 'source image changed'
    print('PASS: source image unchanged; actual IDF FatFs, ASan/UBSan, root regression, subdirectory and 2h capacity')


if __name__ == '__main__':
    main()
