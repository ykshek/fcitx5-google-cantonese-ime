"""Simple Flask daemon exposing /suggest that queries Google Input Tools.

Run: python daemon/server.py
"""
from flask import Flask, request, jsonify
from google_tools import get_suggestions

app = Flask(__name__)


@app.route("/suggest")
def suggest():
    q = request.args.get("q", "")
    if not q:
        return jsonify({"error": "missing q"}), 400
    itc = request.args.get("itc", "zh-t-i0-pinyin")
    try:
        num = int(request.args.get("num", 8))
    except Exception:
        num = 8

    candidates = get_suggestions(q, itc=itc, num=num)
    return jsonify({"query": q, "itc": itc, "candidates": candidates})


if __name__ == "__main__":
    # Bind only to localhost; this is a local helper daemon for the system
    app.run(host="127.0.0.1", port=8765, debug=False)
