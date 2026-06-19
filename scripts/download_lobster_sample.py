"""Fetch LOBSTER sample data into data/lobster/.

LOBSTER (https://lobsterdata.com) publishes free one-day samples for a handful
of tickers at several depths. The official site is now a single-page app whose
old direct file URLs no longer resolve, so this script pulls the same files from
a stable public mirror on the Hugging Face Hub.

Each (ticker, levels) sample is a folder containing two CSVs:

    <TICKER>_<DATE>_<START>_<END>_message_<LEVELS>.csv     the event stream
    <TICKER>_<DATE>_<START>_<END>_orderbook_<LEVELS>.csv    the reference book

The orderbook file is the oracle for `oracle.cpp`: row i is the book state
immediately after event i, as `ask1, asksize1, bid1, bidsize1, ...` out to
<LEVELS> levels. Deeper samples (e.g. 50) cover a shorter time window but buffer
the top of book against the feed's sub-depth data loss.

Usage:
    python scripts/download_lobster_sample.py                 # AAPL, 10 levels
    python scripts/download_lobster_sample.py --levels 50     # AAPL, 50 levels
    python scripts/download_lobster_sample.py --ticker MSFT
"""

import argparse
import json
import sys
import urllib.request
from pathlib import Path

REPO = "totalorganfailure/lobster-data"
API  = f"https://huggingface.co/api/datasets/{REPO}/tree/main"
RAW  = f"https://huggingface.co/datasets/{REPO}/resolve/main"
DATE = "2012-06-21"
DEST = Path(__file__).resolve().parents[1] / "data" / "lobster"


def list_folder(folder: str) -> list[str]:
    with urllib.request.urlopen(f"{API}/{folder}") as resp:  # noqa: S310 (trusted host)
        entries = json.load(resp)
    return [e["path"] for e in entries if e.get("type") == "file"]


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--ticker", default="AAPL")
    ap.add_argument("--levels", type=int, default=10)
    args = ap.parse_args()

    folder = f"LOBSTER_SampleFile_{args.ticker}_{DATE}_{args.levels}"
    try:
        paths = list_folder(folder)
    except Exception as exc:  # noqa: BLE001
        print(f"could not list {folder}: {exc}", file=sys.stderr)
        print(f"browse available folders at https://huggingface.co/datasets/{REPO}/tree/main",
              file=sys.stderr)
        return 2

    wanted = [p for p in paths if p.endswith(".csv") and ("message_" in p or "orderbook_" in p)]
    if not wanted:
        print(f"no message/orderbook CSVs found in {folder}", file=sys.stderr)
        return 2

    DEST.mkdir(parents=True, exist_ok=True)
    for path in wanted:
        out = DEST / Path(path).name
        print(f"downloading {Path(path).name}")
        urllib.request.urlretrieve(f"{RAW}/{path}", out)  # noqa: S310 (trusted host)
        print(f"  -> {out}  ({out.stat().st_size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
