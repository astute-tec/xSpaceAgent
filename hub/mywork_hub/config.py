"""配置：从 ~/.config/mywork-hub/.env 和简单默认值读取。"""
from __future__ import annotations

import os
from pathlib import Path

HOME = Path.home()
ENV_FILE = Path(os.environ.get("MYWORK_ENV", HOME / ".config/mywork-hub/.env"))


def _load_env(path: Path) -> None:
    if not path.exists():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.split("#")[0].strip()
        os.environ.setdefault(key, val)


_load_env(ENV_FILE)

# 注意：必须在 _load_env 之后取默认值，否则 .env 里的 MYWORK_ROOT 不生效
DEFAULT_ROOT = Path(os.environ.get("MYWORK_ROOT", HOME / "Mywork"))


class Config:
    root: Path = DEFAULT_ROOT
    work_dirname: str = os.environ.get("MYWORK_DIRNAME", "05_语音工作日志")

    whisper_bin: str = os.environ.get("WHISPER_BIN", "whisper-cli")
    whisper_model: str = os.environ.get(
        "WHISPER_MODEL", str(HOME / ".cache/whisper/ggml-medium.bin")
    )

    deepseek_key: str = os.environ.get("DEEPSEEK_API_KEY", "")
    deepseek_base: str = os.environ.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com/v1")
    model_fast: str = os.environ.get("LLM_MODEL_FAST", "deepseek-flash")
    model_pro: str = os.environ.get("LLM_MODEL_PRO", "deepseek-v4-pro")

    # ---- 阿里云语音 ----
    aliyun_key_id: str = os.environ.get("ALIYUN_ACCESS_KEY_ID", "")
    aliyun_secret: str = os.environ.get("ALIYUN_ACCESS_KEY_SECRET", "")
    aliyun_appkey: str = os.environ.get("ALIYUN_NLS_APPKEY", "")
    asr_provider: str = os.environ.get("ASR_PROVIDER", "local")   # local | aliyun
    aliyun_region: str = os.environ.get("ALIYUN_REGION", "cn-shanghai")

    filter_threshold: float = float(os.environ.get("FILTER_THRESHOLD", "0.6"))
    review_threshold: float = float(os.environ.get("REVIEW_THRESHOLD", "0.4"))

    @property
    def work_dir(self) -> Path:
        return self.root / self.work_dirname


cfg = Config()
