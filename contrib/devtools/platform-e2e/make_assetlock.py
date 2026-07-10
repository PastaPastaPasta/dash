#!/usr/bin/env python3
"""Create + broadcast a TRANSACTION_ASSET_LOCK on testnet funding a single
credit output to a given P2PKH pubkey-hash, then wait for its InstantSend
lock. Prints JSON: {txid, tx_hex, islock_hex, output_index}.

Reuses the functional-test serialization classes but talks to a live
testnet dashd over RPC (no test framework node needed)."""
import json
import subprocess
import sys
import time
import os

FW = "/Users/pasta/workspace/dash/.claude/worktrees/dash-platform-usernames-ui-bbe2b0/test/functional"
sys.path.insert(0, FW)
from test_framework.messages import (  # noqa: E402
    CTransaction, CTxIn, CTxOut, COutPoint, CAssetLockTx, COIN, tx_from_hex,
)
from test_framework.script import CScript, OP_RETURN  # noqa: E402
from test_framework.address import key_to_p2pkh, script_to_p2sh  # noqa: E402

DATADIR = os.path.expanduser("~/dash-testnet-e2e")
CLI = "/Users/pasta/workspace/dash/.claude/worktrees/dash-platform-usernames-ui-bbe2b0/src/dash-cli"


def cli(*args, wallet="e2e"):
    cmd = [CLI, "-testnet", f"-datadir={DATADIR}"]
    if wallet:
        cmd.append(f"-rpcwallet={wallet}")
    cmd += list(args)
    out = subprocess.check_output(cmd, text=True).strip()
    return out


def key_to_p2pkh_script(pubkey_hash_hex):
    # OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG
    from test_framework.script import OP_DUP, OP_HASH160, OP_EQUALVERIFY, OP_CHECKSIG
    return CScript([OP_DUP, OP_HASH160, bytes.fromhex(pubkey_hash_hex), OP_EQUALVERIFY, OP_CHECKSIG])


def main():
    pubkey_hash = sys.argv[1]        # 20-byte hex, the identity funding key hash
    amount = int(sys.argv[2])        # duffs for the single credit output

    # Build a bare asset-lock tx with just the OP_RETURN burn output + payload;
    # fundrawtransaction adds inputs and change.
    credit_outputs = [CTxOut(amount, key_to_p2pkh_script(pubkey_hash))]
    payload = CAssetLockTx(1, credit_outputs)

    tx = CTransaction()
    tx.nVersion = 3
    tx.nType = 8
    tx.vout = [CTxOut(amount, CScript([OP_RETURN, b""]))]
    tx.vExtraPayload = payload.serialize()

    raw = tx.serialize().hex()

    # Fund (adds inputs + change), preserving nType/payload.
    funded = json.loads(cli("fundrawtransaction", raw, json.dumps({"feeRate": 0.00005})))
    signed = json.loads(cli("signrawtransactionwithwallet", funded["hex"]))
    assert signed.get("complete"), signed
    signed_hex = signed["hex"]

    # Confirm the OP_RETURN output index (it should stay at 0; find it anyway).
    ftx = tx_from_hex(signed_hex)
    op_return_vout = None
    for i, o in enumerate(ftx.vout):
        spk = bytes(o.scriptPubKey)
        if spk and spk[0] == 0x6a:  # OP_RETURN
            op_return_vout = i
            break

    txid = cli("sendrawtransaction", signed_hex)

    # Wait for the InstantSend lock.
    islock_hex = None
    for _ in range(120):
        info = json.loads(cli("getrawtransaction", txid, "true"))
        if info.get("instantlock") and info.get("instantlock_internal"):
            # fetch the raw islock
            try:
                islock_hex = cli("getislocks", json.dumps([txid]))
                il = json.loads(islock_hex)
                if il and il[0].get("hex"):
                    islock_hex = il[0]["hex"]
                    break
            except subprocess.CalledProcessError:
                pass
        time.sleep(2)

    print(json.dumps({
        "txid": txid,
        "tx_hex": signed_hex,
        "islock_hex": islock_hex,
        "op_return_vout": op_return_vout,
        # The identity-funding credit output is index 0 within the payload.
        "asset_lock_output_index": 0,
    }))


if __name__ == "__main__":
    main()
