#!/usr/bin/env python3
"""Drive the thepasta testnet faucet's own page to fund an address.
The page's own JS solves the Cap.js PoW; we just reveal the Core faucet form,
fill the address, and click Send."""
import sys, json, re
from playwright.sync_api import sync_playwright

ADDRESS = sys.argv[1] if len(sys.argv) > 1 else "ybsyFQdrM65TT52T9MwMT7isWu3tx5NRN3"
URL = "https://faucet.thepasta.org/"


def main():
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        page = browser.new_context().new_page()
        captured = {"resp": None, "txid": None}

        def on_response(resp):
            if "core-faucet" in resp.url:
                try:
                    body = resp.text()
                    captured["resp"] = (resp.status, body)
                    try:
                        j = json.loads(body)
                    except Exception:
                        j = {}
                    for k in ("txid", "txId", "transactionId", "tx", "hash"):
                        v = j.get(k) if isinstance(j, dict) else None
                        if isinstance(v, str) and re.fullmatch(r"[0-9a-fA-F]{64}", v):
                            captured["txid"] = v
                    if not captured["txid"]:
                        m = re.search(r'[0-9a-fA-F]{64}', body)
                        if m:
                            captured["txid"] = m.group(0)
                except Exception as e:
                    captured["resp"] = ("err", str(e))

        page.on("response", on_response)
        page.goto(URL, wait_until="networkidle", timeout=60000)

        # Reveal the Core faucet form if the address field is hidden.
        if not page.is_visible("#addressInput"):
            for txt in ["Get tDash", "Core Wallet", "tDash"]:
                try:
                    page.click(f'button:has-text("{txt}")', timeout=3000)
                    break
                except Exception:
                    continue
        page.wait_for_selector("#addressInput", state="visible", timeout=15000)
        page.fill("#addressInput", ADDRESS)
        page.click("#coreFaucetBtn")

        # Wait for the faucet API response (PoW solve can take up to ~2 min).
        for _ in range(150):
            if captured["resp"] is not None:
                break
            page.wait_for_timeout(1000)

        if not captured["txid"]:
            try:
                m = re.search(r'[0-9a-fA-F]{64}', page.content())
                if m:
                    captured["txid"] = m.group(0)
            except Exception:
                pass
        browser.close()

        if captured["txid"]:
            print("SUCCESS", captured["txid"])
            return 0
        print("FAILED", captured["resp"])
        return 1


if __name__ == "__main__":
    sys.exit(main())
