"""Fetch the free LOBSTER sample data into data/lobster/.

LOBSTER (https://lobsterdata.com/info/DataSamples.php) publishes free one-day
samples for a few tickers at several depth levels. Each sample is a zip
containing two CSVs:

    <TICKER>_<DATE>_..._message_<LEVELS>.csv     the event stream
    <TICKER>_<DATE>_..._orderbook_<LEVELS>.csv    the reference book snapshots

The orderbook file is the oracle: row i is the book state immediately after
event i in the message file, as `ask1, asksize1, bid1, bidsize1, ...` out to
<LEVELS> levels. The planned oracle test replays the message file and asserts
the engine's top-N levels match this file row for row.

Usage:
    python scripts/download_lobster_sample.py            # default: AAPL level 10
    python scripts/download_lobster_sample.py --list     # show known samples
"""

import argparse
import io
import sys
import urllib.request
import zipfile
from pathlib import Path

# Known free sample zips (ticker -> {levels: url}). LOBSTER hosts these on its
# public sample page; URLs are stable but verify against the page if a fetch
# 404s, since they revise occasionally.
SAMPLES = {
    "AAPL": {
        1: "https://lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_1.zip",
        10: "https://lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_10.zip",
        50: "https://lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_50.zip",
    },
    "MSFT": {
        1: "https://lobsterdata.com/info/sample/LOBSTER_SampleFile_MSFT_2012-06-21_1.zip",
        10: "https://lobsterdata.com/info/sample/LOBSTER_SampleFile_MSFT_2012-06-21_10.zip",
    },
}

DEST = Path(__file__).resolve().parents[1] / "data" / "lobster"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ticker", default="AAPL", choices=sorted(SAMPLES))
    ap.add_argument("--levels", type=int, default=10)
    ap.add_argument("--list", action="store_true", help="list known samples and exit")
    args = ap.parse_args()

    if args.list:
        for tk, lv in SAMPLES.items():
            print(f"{tk}: levels {sorted(lv)}")
        return 0

    try:
        url = SAMPLES[args.ticker][args.levels]
    except KeyError:
        print(f"no known sample for {args.ticker} level {args.levels}; try --list", file=sys.stderr)
        return 2

    DEST.mkdir(parents=True, exist_ok=True)
    print(f"downloading {url}")
    with urllib.request.urlopen(url) as resp:  # noqa: S310 (trusted host)
        blob = resp.read()
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        z.extractall(DEST)
        names = z.namelist()
    print(f"extracted {len(names)} file(s) to {DEST}:")
    for n in names:
        print(f"  {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
