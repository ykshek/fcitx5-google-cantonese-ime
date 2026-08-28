"""Helpers to query Google Input Tools.

This module provides a small wrapper to call the inputtools endpoint and
parse candidate responses.
"""
import json
import urllib.parse
import requests

GOOGLE_INPUTTOOLS_URL = "https://inputtools.google.com/request"


def get_suggestions(text: str, itc: str = "yue-hant-t-i0-und", num: int = 8):
    """Query Google Input Tools and return a list of candidate strings.

    Args:
        text: query text (raw input from user)
        itc: input tools code, e.g. "zh-t-i0-pinyin" or a cantonese code if available
        num: number of candidates requested

    Returns:
        List[str] of candidate strings (may be empty)
    """
    params = {
        "text": text,
        "itc": itc,
        "num": num,
    }
    try:
        resp = requests.get(GOOGLE_INPUTTOOLS_URL, params=params, timeout=5)
        resp.raise_for_status()
        data = resp.json()
    except Exception:
        return []

    # Response shape is typically: [status, [[original_text, [candidates...], ...]]]
    try:
        # some responses place candidates at data[1][0][1]
        candidates = data[1][0][1]
        if isinstance(candidates, list):
            return [str(c) for c in candidates]
    except Exception:
        pass

    # Fallback: scan the JSON for strings
    def scan(obj):
        if isinstance(obj, str):
            return [obj]
        if isinstance(obj, list):
            out = []
            for item in obj:
                out.extend(scan(item))
            return out
        if isinstance(obj, dict):
            out = []
            for v in obj.values():
                out.extend(scan(v))
            return out
        return []

    return scan(data)
