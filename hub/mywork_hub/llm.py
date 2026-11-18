"""DeepSeek（OpenAI 兼容）最小客户端，只依赖标准库。"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Any

from .config import cfg


class LLMError(RuntimeError):
    pass


def chat(
    messages: list[dict[str, str]],
    *,
    model: str | None = None,
    temperature: float = 0.0,
    json_mode: bool = False,
    timeout: int = 120,
    retries: int = 3,
) -> str:
    if not cfg.deepseek_key:
        raise LLMError("缺少 DEEPSEEK_API_KEY（~/.config/mywork-hub/.env）")

    payload: dict[str, Any] = {
        "model": model or cfg.model_fast,
        "messages": messages,
        "temperature": temperature,
        "stream": False,
    }
    if json_mode:
        payload["response_format"] = {"type": "json_object"}

    body = json.dumps(payload).encode()
    url = f"{cfg.deepseek_base.rstrip('/')}/chat/completions"

    last: Exception | None = None
    for attempt in range(retries):
        req = urllib.request.Request(
            url,
            data=body,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {cfg.deepseek_key}",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = json.loads(resp.read().decode())
            return data["choices"][0]["message"]["content"]
        except urllib.error.HTTPError as exc:  # 4xx/5xx 带 body，便于排查
            detail = exc.read().decode("utf-8", "replace")[:400]
            last = LLMError(f"HTTP {exc.code}: {detail}")
            if exc.code in (400, 401, 403, 404):
                raise last from exc
        except Exception as exc:  # noqa: BLE001
            last = exc
        if attempt < retries - 1:
            import time

            time.sleep(1.5 * (attempt + 1))
    raise LLMError(f"调用失败: {last}")


def chat_json(messages: list[dict[str, str]], **kw) -> Any:
    text = chat(messages, json_mode=True, **kw)
    text = text.strip()
    if text.startswith("```"):
        text = text.split("```")[1]
        text = text.removeprefix("json").strip()
    return json.loads(text)
