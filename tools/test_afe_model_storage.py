#!/usr/bin/env python3
"""Check the real AFE model relocation, including mapped-pointer lifetimes."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1]/'firmware/wifi-provision/main/afe.c').read_text()
start = source.index('static bool models_move_to_psram(')
end = source.index('\n#define RING_SIZE', start)
function = source[start:end]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define SRMODEL_STRING_LENGTH 32
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGI(...) ((void)0)
typedef unsigned esp_partition_mmap_handle_t;
typedef struct { int num; char **files; char **data; int *sizes; } srmodel_data_t;
typedef struct { int num; void *mmap_handle; srmodel_data_t **model_data; } srmodel_list_t;
static void *s_model_storage;
static bool fail_alloc;
static unsigned unmapped;
static void *heap_caps_aligned_alloc(size_t align,size_t size,int caps) {
    return fail_alloc ? NULL : aligned_alloc(align,size);
}
static void esp_partition_munmap(unsigned handle) { assert(handle==77); unmapped++; }
'''
harness += function
harness += r'''
int main(void) {
    assert(models_move_to_psram(NULL));
    srmodel_list_t empty={0}; assert(models_move_to_psram(&empty));
    unsigned char mapped[128]={0};
    memcpy(mapped,"model_data",11); memcpy(mapped+32,"_MODEL_INFO_",13);
    for(int i=0;i<17;i++) mapped[64+i]=(unsigned char)i;
    memcpy(mapped+96,"test info",10);
    char *names[]={(char *)mapped,(char *)mapped+32};
    char *data[]={(char *)mapped+64,(char *)mapped+96};
    int sizes[]={17,10};
    srmodel_data_t m={.num=2,.files=names,.data=data,.sizes=sizes};
    srmodel_data_t *table[]={&m};
    unsigned *handle=malloc(sizeof(*handle)); *handle=77;
    srmodel_list_t models={.num=1,.mmap_handle=handle,.model_data=table};
    fail_alloc=true; assert(!models_move_to_psram(&models));
    assert(unmapped==0 && models.mmap_handle==handle && names[0]==(char *)mapped);
    fail_alloc=false; sizes[1]=-1; assert(!models_move_to_psram(&models));
    assert(unmapped==0 && names[0]==(char *)mapped); sizes[1]=10;
    assert(models_move_to_psram(&models));
    assert(unmapped==1 && models.mmap_handle==NULL && s_model_storage);
    memset(mapped,0xcc,sizeof(mapped)); /* Original mapping no longer accessible. */
    assert(!strcmp(names[0],"model_data") && !strcmp(names[1],"_MODEL_INFO_"));
    for(int i=0;i<17;i++) assert((unsigned char)data[0][i]==i);
    assert(!memcmp(data[1],"test info",10));
    assert((uintptr_t)data[0]%16==0 && (uintptr_t)data[1]%16==0);
    assert(models_move_to_psram(&models) && unmapped==1);
    free(s_model_storage);
    puts("PASS: model names/data survive unmap, aligned payloads, allocation/metadata failure keeps original mapping");
}
'''
with tempfile.TemporaryDirectory(prefix='afe-model-storage-') as td:
    tmp = Path(td)
    (tmp/'test.c').write_text(harness)
    subprocess.run(['clang','-O1','-fsanitize=address,undefined',str(tmp/'test.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
