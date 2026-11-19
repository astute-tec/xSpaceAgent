#!/usr/bin/env python3
"""Run the real Ogg writer with libopus + ASan/UBSan, then decode with ffmpeg.

Host codec adapter exercises the official ESP encoder API boundary. This does
not measure ESP CPU load or replace a two-hour device endurance recording.
"""
from pathlib import Path
import json
import math
import subprocess
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/wifi-provision/main"
CODEC = ROOT / "firmware/wifi-provision/managed_components/espressif__esp_audio_codec"
OUTPUT = ROOT / "out/offline-recording-validation"
OUTPUT.mkdir(parents=True, exist_ok=True)

ADAPTER = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus.h>
#include "esp_opus_enc.h"
#include "recording_codec.h"
esp_audio_err_t esp_opus_enc_open(void *p,uint32_t n,void **out) {
    esp_opus_enc_config_t *c=p; int err;
    OpusEncoder *e=opus_encoder_create(c->sample_rate,c->channel,OPUS_APPLICATION_VOIP,&err);
    if(!e || err) return ESP_AUDIO_ERR_FAIL;
    assert(!opus_encoder_ctl(e,OPUS_SET_BITRATE(c->bitrate)));
    assert(!opus_encoder_ctl(e,OPUS_SET_VBR(c->enable_vbr)));
    assert(!opus_encoder_ctl(e,OPUS_SET_DTX(c->enable_dtx)));
    assert(!opus_encoder_ctl(e,OPUS_SET_COMPLEXITY(c->complexity)));
    *out=e; return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_enc_get_frame_size(void *e,int *in,int *out) {
    *in=640; *out=1275; return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_enc_get_info(void *e,esp_audio_enc_info_t *info) {
    int delay; opus_encoder_ctl(e,OPUS_GET_LOOKAHEAD(&delay));
    /* Exercise fallback too: the ESP wrapper need not expose OpusHead. */
    assert(delay*3==312); memset(info,0,sizeof(*info)); return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_enc_process(void *e,esp_audio_enc_in_frame_t *in,esp_audio_enc_out_frame_t *out) {
    int n=opus_encode(e,(const opus_int16 *)in->buffer,in->len/2,out->buffer,out->len);
    if(n<0) return ESP_AUDIO_ERR_FAIL;
    out->encoded_bytes=n; return ESP_AUDIO_ERR_OK;
}
void esp_opus_enc_close(void *e) { opus_encoder_destroy(e); }
int main(int argc,char **argv) {
    assert(argc==3); FILE *in=fopen(argv[1],"rb"); assert(in);
    fseek(in,0,SEEK_END); long bytes=ftell(in); rewind(in);
    int16_t *pcm=malloc(bytes); assert(fread(pcm,1,bytes,in)==bytes); fclose(in);
    uint8_t *out=0; size_t size=0;
    assert(recording_encode(pcm,bytes/2,1234,&out,&size)==0);
    FILE *f=fopen(argv[2],"wb"); assert(f); assert(fwrite(out,1,size,f)==size); fclose(f);
    printf("samples=%ld bytes=%zu\n",bytes/2,size);
    free(out); free(pcm);
    assert(recording_encode(NULL,1,1,&out,&size)!=0 && !out && !size);
}
'''

with tempfile.TemporaryDirectory(prefix="recording-codec-") as temp:
    tmp = Path(temp)
    (tmp / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG 258\n#define ESP_ERR_NO_MEM 257\n")
    (tmp / "esp_heap_caps.h").write_text("#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\n#define heap_caps_malloc(n,c) malloc(n)\n")
    (tmp / "adapter.c").write_text(ADAPTER)
    args = ["clang", "-O1", "-g", "-fsanitize=address,undefined", "-I", str(tmp), "-I", str(MAIN),
            "-I", str(CODEC / "include"), "-I", str(CODEC / "include/encoder"),
            "-I", str(CODEC / "include/encoder/impl"), "-I", "/opt/homebrew/opt/opus/include/opus",
            str(tmp / "adapter.c"), str(MAIN / "recording_codec.c"),
            "-L", "/opt/homebrew/opt/opus/lib", "-lopus", "-o", str(tmp / "encode")]
    subprocess.run(args, check=True)
    rng = np.random.default_rng(123)
    measurements = []
    # Full speech/noise/silence segments plus short tails and maximum question length.
    for seconds, kind in [(20, "voice"), (20, "noise"), (20, "silence"), (0.1, "voice"), (1.001, "voice"), (30, "voice")]:
        count = int(seconds * 16000)
        if kind == "silence":
            pcm = np.zeros(count, dtype="<i2")
        elif kind == "noise":
            pcm = rng.integers(-10000, 10000, count, dtype="<i2")
        else:
            original = np.frombuffer((MAIN / "audio/no_speech.pcm").read_bytes(), dtype="<i2").reshape(-1, 2)[:, 0]
            pcm = np.resize(original, count).astype("<i2")
        src = tmp / "source.pcm"; src.write_bytes(pcm.tobytes())
        encoded = OUTPUT / f"{kind}-{seconds}.opus"
        subprocess.run([str(tmp / "encode"), str(src), str(encoded)], check=True)
        decoded = subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(encoded), "-ar", "16000", "-ac", "1", "-f", "s16le", "-"], check=True, capture_output=True).stdout
        assert len(decoded) == count * 2, (seconds, len(decoded), count * 2)
        budget = math.ceil(encoded.stat().st_size / 8192) * 8192
        if seconds == 20:
            assert encoded.stat().st_size <= 54000
            assert budget * 360 + 1024**2 < 0x19F0000
        measurements.append({"seconds": seconds, "kind": kind, "bytes": encoded.stat().st_size,
                             "allocated_8KiB": budget, "decoded_samples": len(decoded)//2})
    segment_alloc = math.ceil(54000 / 8192) * 8192
    result = {"measurements": measurements, "two_hour_segments": 360,
              "two_hour_budget_bytes": segment_alloc*360,
              "partition_bytes": 0x19F0000, "reserve_bytes": 1024**2,
              "theoretical_upper_seconds_before_fs_overhead": (0x19F0000-1024**2)//segment_alloc*20,
              "device_endurance_verified": False}
    (OUTPUT / "capacity.json").write_text(json.dumps(result, ensure_ascii=False, indent=2)+"\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    print("PASS: real Ogg writer, Opus CBR, CRC decode, exact sample counts, silence and 2h allocation budget")
