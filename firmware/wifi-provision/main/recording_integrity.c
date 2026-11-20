#include "recording_integrity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static uint32_t crc_bytes(uint32_t crc, const uint8_t *bytes, size_t size)
{
    for (size_t i=0; i<size; i++) {
        crc ^= (uint32_t)bytes[i]<<24;
        for (int bit=0; bit<8; bit++)
            crc = (crc<<1) ^ ((crc & 0x80000000U) ? 0x04c11db7U : 0);
    }
    return crc;
}

bool recording_file_complete(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t serial=0, sequence=0;
    bool ok=false, eos=false;
    uint8_t header[27], lacing[255], buffer[256];
    for (;;) {
        size_t n=fread(header,1,sizeof(header),f);
        if (!n) { ok=!ferror(f) && eos && sequence>=3; break; }
        if (n!=sizeof(header) || eos || memcmp(header,"OggS",4) || header[4]!=0) break;
        if (!sequence) serial=le32(header+14);
        if (le32(header+14)!=serial || le32(header+18)!=sequence ||
            (!sequence && !(header[5]&2)) || (sequence && (header[5]&2))) break;
        uint32_t expected=le32(header+22);
        memset(header+22,0,4);
        uint32_t crc=crc_bytes(0,header,sizeof(header));
        size_t segments=header[26];
        if (!segments || fread(lacing,1,segments,f)!=segments) break;
        crc=crc_bytes(crc,lacing,segments);
        size_t payload=0;
        for (size_t i=0; i<segments; i++) payload+=lacing[i];
        bool first=true, bad=false;
        while (payload) {
            size_t bytes=payload<sizeof(buffer)?payload:sizeof(buffer);
            if (fread(buffer,1,bytes,f)!=bytes) { bad=true; break; }
            if (!sequence && first && (bytes<19 || memcmp(buffer,"OpusHead",8))) { bad=true; break; }
            crc=crc_bytes(crc,buffer,bytes);
            payload-=bytes; first=false;
        }
        if (bad || crc!=expected || (!sequence && first)) break;
        eos=(header[5]&4)!=0;
        sequence++;
        /* Recordings are bounded; reject runaway/corrupt chains quickly. */
        if (sequence>2048) break;
    }
    if (fclose(f)!=0) ok=false;
    return ok;
}
