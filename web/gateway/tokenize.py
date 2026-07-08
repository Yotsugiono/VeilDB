"""
将原始邮件/文本转为 Enron 风格的逗号分隔词干串（供 UpdateKeywords / 倒排索引）。
逻辑对齐 EncDB/data/enron_gen.py；无 NLTK 时使用轻量回退。
"""
from __future__ import annotations

import re
from typing import List, Set

_HEADER_MARKERS = (
    "Message-ID:",
    "Date:",
    "From:",
    "To:",
    "Subject:",
    "Cc:",
    "Bcc:",
    "X-From:",
    "X-To:",
    "X-cc:",
    "X-bcc:",
    "X-Folder:",
    "X-Origin:",
    "X-FileName:",
    "Mime-Version:",
    "Content-Type:",
    "charset=us",
    "Content-Transfer-Encoding",
)

_STOP_WORDS = {
    "a", "an", "the", "and", "or", "but", "in", "on", "at", "to", "for", "of",
    "as", "by", "with", "from", "is", "was", "are", "were", "be", "been", "it",
    "this", "that", "i", "you", "he", "she", "we", "they", "my", "your", "his",
    "her", "our", "their", "not", "no", "do", "does", "did", "have", "has", "had",
    "will", "would", "can", "could", "should", "may", "might", "must", "am",
}


def _strip_headers(text: str) -> str:
    lines = text.splitlines()
    body: List[str] = []
    in_body = False
    for line in lines:
        if not in_body:
            if any(line.startswith(m) for m in _HEADER_MARKERS):
                continue
            if line.strip() == "":
                in_body = True
                continue
        body.append(line)
    return "\n".join(body) if body else text


def _simple_stem(word: str) -> str:
    w = word.lower()
    for suffix in ("ing", "ed", "es", "s"):
        if len(w) > 4 and w.endswith(suffix):
            return w[: -len(suffix)]
    return w


def _tokenize_fallback(text: str) -> List[str]:
    text = _strip_headers(text)
    words = re.findall(r"[A-Za-z]{2,}", text)
    seen: Set[str] = set()
    out: List[str] = []
    for w in words:
        stem = _simple_stem(w)
        if stem in _STOP_WORDS or stem in seen:
            continue
        seen.add(stem)
        out.append(stem)
    return out


def _tokenize_nltk(text: str) -> List[str]:
    import nltk
    from nltk.corpus import stopwords
    from nltk.stem import PorterStemmer

    text = _strip_headers(text)
    tokens = nltk.word_tokenize(text)
    stop = set(stopwords.words("english"))
    stemmer = PorterStemmer()
    seen: Set[str] = set()
    out: List[str] = []
    for tok in tokens:
        if not re.match(r"^[A-Za-z]+$", tok):
            continue
        w = stemmer.stem(tok.lower())
        if w in stop or len(w) < 2 or w in seen:
            continue
        seen.add(w)
        out.append(w)
    return out


def build_index_payload(text: str) -> str:
    """返回逗号分隔词干串（无尾部逗号）。"""
    try:
        words = _tokenize_nltk(text)
    except Exception:
        words = _tokenize_fallback(text)
    return ",".join(words)


def truncate_raw_document(raw: bytes, max_len: int) -> tuple[bytes, bool]:
    if len(raw) <= max_len:
        return raw, False
    return raw[:max_len], True
