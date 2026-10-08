#!/usr/bin/env python3
"""Look up JLCPCB/LCSC stock and price for the PCBA parts (read-only, no account).

    python3 tools/jlc_stock.py            # writes hw/results/jlc_stock.json and prints a table

Uses JLCPCB's public parts-search endpoint (the one the parts library page calls). Prices
are USD at qty 1 / 10. "basic" parts have no setup fee; each "extended" part adds ~$3 per order.
"""
import json, os, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "hw", "results", "jlc_stock.json")
URL = "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList"

# keyword -> substring the manufacturer part number must contain
QUERIES = [
    ("PCM3168A", "PCM3168APAPR"), ("DAC8568", "DAC8568"), ("ADS131M08", "ADS131M08"),
    ("OPA4172IDR", "OPA4172"), ("OPA1679IPWR", "OPA1679"), ("TL074CDR", "TL074"),
    ("TPS7A4901DGNR", "TPS7A4901"), ("TPS7A3001DGNR", "TPS7A3001"),
    ("LP5907MFX-4.5", "LP5907MFX-4.5"), ("LP5907MFX-3.3", "LP5907MFX-3.3"), ("AMS1117-3.3", "AMS1117-3.3"),
    ("SS14", "SS14"), ("BAT54S", "BAT54S"), ("B0515S-2WR3", "B0515S"),
    ("ERA-6AEB103V", "ERA-6AEB103"), ("ERA-6AEB8252V", "ERA-6AEB8252"), ("ERA-6AEB8061V", "ERA-6AEB8061"),
    ("ERA-6AEB203V", "ERA-6AEB203"), ("ERA-6AEB5762V", "ERA-6AEB5762"),
    ("RC0805FR-0749K9L", "49K9"), ("RC0805FR-079K09L", "9K09"), ("RC0805FR-0711KL", "11K"),
    ("RC0805FR-078K2L", "8K2"), ("ERJ-P08J102V", "ERJ-P08J102"),
    ("SWPA4030S100MT", "SWPA4030S100"),
]


def search(keyword, base=False):
    # curl with a hard deadline: urllib's timeout does not cover every stall on this endpoint
    import subprocess
    q = {"keyword": keyword, "currentPage": 1, "pageSize": 25}
    if base:
        q["componentLibraryType"] = "base"
    body = json.dumps(q)
    out = subprocess.run(["curl", "-s", "-m", "45", "-A", "Mozilla/5.0", "-H", "content-type: application/json",
                          "--data", body, URL], capture_output=True, text=True, timeout=60).stdout
    d = json.loads(out or "{}")
    return ((d.get("data") or {}).get("componentPageInfo") or {}).get("list") or []


def best(rows, must):
    rows = [r for r in rows if must.upper() in (r.get("componentModelEn") or "").upper()] if must else rows
    rows.sort(key=lambda r: (r.get("componentLibraryType") != "base", -(r.get("stockCount") or 0)))
    return rows[0] if rows else None


EXTRA = [("A0515S-2WR3", "A0515S"), ("TLV9064IPWR", "TLV9064"), ("LM66100DCKR", "LM66100"),
         ("8.192MHz crystal 3225", "8.192"), ("10uF 0805 25V", ""), ("22uF 0805", ""),
         ("10kΩ 0805 ±0.1%", ""), ("82.5kΩ 0805 ±0.1%", ""), ("8.06kΩ 0805 ±0.1%", ""),
         ("20kΩ 0805 ±0.1%", ""), ("57.6kΩ 0805 ±0.1%", ""),
         ("RT0805BRD0757K6L", "57K6"), ("RT0805BRD0720KL", "20K"), ("RT0805BRD0782K5L", "82K5"),
         ("28.7kΩ 0805 ±0.1%", ""), ("27.4kΩ 0805 ±0.1%", ""), ("30.1kΩ 0805 ±0.1%", ""), ("29.4kΩ 0805 ±0.1%", ""),
         ("100nF 0805 X7R 50V", ""), ("1uF 0805 X7R 25V", ""), ("10nF 0805 X7R", ""), ("220nF 0805 X7R", ""),
         ("100pF 0805 C0G", ""), ("12pF 0805 C0G", ""), ("47pF 0805 C0G", ""), ("56pF 0805 C0G", ""),
         ("330pF 0805 C0G", ""), ("33kΩ 0805 ±1%", ""), ("10kΩ 0805 ±1%", ""), ("100kΩ 0805 ±1%", ""),
         ("100Ω 0805 ±1%", "")]


def main():
    res = []
    queries = QUERIES
    old = {}
    if len(sys.argv) > 1 and sys.argv[1] == "extra":       # query EXTRA only and merge into the existing file
        queries = EXTRA
        try:
            with open(OUT) as f:
                old = {p["query"]: p for p in json.load(f)["parts"]}
        except Exception:
            old = {}
    from concurrent.futures import ThreadPoolExecutor
    def fetch(q):
        try:
            if not q[1]:          # generic passive: prefer a JLC basic part in the right package
                pkg = "0805" if "0805" in q[0] else ""
                tok = q[0].split()[0]                      # value, e.g. "10nF", "33kΩ"
                def ok(r):
                    if pkg not in (r.get("componentSpecificationEn") or ""):
                        return False
                    attrs = {x.get("attribute_name_en"): (x.get("attribute_value_name") or "").replace(" ", "")
                             for x in (r.get("attributes") or [])}
                    return attrs.get("Capacitance") == tok or attrs.get("Resistance") == tok
                rows = [r for r in search(f"{tok} {pkg}", base=True) if ok(r)]
                if not rows:
                    rows = [r for r in search(q[0]) if ok(r)]
                if rows:
                    return q, rows, None
            return q, search(q[0]), None
        except Exception as e:  # network or API change
            return q, None, e
    with ThreadPoolExecutor(4) as ex:
        fetched = list(ex.map(fetch, queries))
    for (kw, must), rows, err in fetched:
        if err is not None:
            res.append({"query": kw, "error": str(err)})
            continue
        r = best(rows, must)
        if not r:
            res.append({"query": kw, "found": False})
            continue
        prices = [(p.get("startNumber"), p.get("productPrice")) for p in (r.get("componentPrices") or [])]
        res.append({"query": kw, "lcsc": r.get("componentCode"), "mpn": r.get("componentModelEn"),
                    "package": r.get("componentSpecificationEn"), "library": r.get("componentLibraryType"),
                    "stock": r.get("stockCount"), "usd_qty1": prices[0][1] if prices else None,
                    "usd_qty10": prices[1][1] if len(prices) > 1 else None})
    if old:
        for x in res:
            old[x["query"]] = x
        res = list(old.values())
    res_meta = {"fetched": time.strftime("%Y-%m-%d %H:%M"), "parts": res}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        json.dump(res_meta, f, indent=1)
    for x in res:
        if "lcsc" in x:
            print(f"{x['query']:22s} {x['lcsc']:10s} {x['mpn']:22s} {x['library']:6s} stock {x['stock']:>7} ${x['usd_qty1']}")
        else:
            print(f"{x['query']:22s} -- {x.get('error', 'not found')}")


if __name__ == "__main__":
    sys.exit(main())
