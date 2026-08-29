#!/usr/bin/env python3
"""Set a number entity on the device over the ESPHome native API.

Usage: set_number.py "Chlorine Output" 30 [--host pool-controller.local]
"""
import argparse
import asyncio
import re
from pathlib import Path

from aioesphomeapi import APIClient

SECRETS = Path(__file__).resolve().parent.parent / "esphome" / "secrets.yaml"


def api_key() -> str:
    m = re.search(r"^api_key:\s*(\S+)", SECRETS.read_text(), re.M)
    if not m:
        raise SystemExit(f"api_key not found in {SECRETS}")
    return m.group(1).strip("\"'")


async def main(host: str, name: str, value: float) -> None:
    cli = APIClient(host, 6053, None, noise_psk=api_key())
    await cli.connect(login=True)
    try:
        entities, _ = await cli.list_entities_services()
        match = [e for e in entities if getattr(e, "name", None) == name]
        if not match:
            raise SystemExit(f"no entity named {name!r}")
        key = match[0].key
        cli.number_command(key, value)
        # The command is fire-and-forget; read the state back to confirm.
        done = asyncio.get_running_loop().create_future()

        def on_state(state):
            if state.key == key and not done.done():
                done.set_result(state.state)

        cli.subscribe_states(on_state)
        print(f"{name} = {await asyncio.wait_for(done, 15)}")
    finally:
        await cli.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("name", help="number entity name")
    parser.add_argument("value", type=float)
    parser.add_argument("--host", default="pool-controller.local")
    args = parser.parse_args()
    asyncio.run(main(args.host, args.name, args.value))
