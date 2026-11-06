# Commands

Binary: `~/work/Boblox/build/client/roblox` · source: `~/work/Boblox`

## Join headless

```sh
cd ~/work/Boblox
./tools/run.sh 1818 --seconds 60
```

`tools/run.sh` reads **Boblox's own cookie** (`~/.config/boblox-cookie`), separate
from Rum's and the executor's. Set it with `./tools/cookie.sh` (hidden input,
writes mode 600); inspect with `./tools/cookie.sh --show`. Override the path with
`$BOBLOX_COOKIE_FILE`, or pass `--cookie-file FILE` directly.

Raw form, if you prefer explicit flags:

```sh
./build/client/roblox --connection-type=RakNet --place-id 1818 \
  --headless --cookie-file ~/.config/boblox-cookie --seconds 60
```

Cookie may come from `$ROBLOX_ROBLOSECURITY` or `$RBX_ROBLOSECURITY` instead of
argv (keeps it out of `ps`).

## Run a user script

```sh
./build/client/roblox --connection-type=RakNet --place-id 286090429 \
  --headless --exec run/live_test.lua --seconds 100
```

Scripts run once the world applies, on the engine scheduler (`task.wait` works).

## Push scripts into a running client

The headless client has no renderer, so the "UI" is a Unix socket — same idea as
the executor's, but it queues and runs on the engine's Lua thread.

```sh
BOBLOX_SOCKET=/tmp/boblox.sock ./tools/run.sh 1818 --seconds 60
./tools/push.sh script.lua          # from another terminal
echo 'print("hi")' | ./tools/push.sh -
```

Each connection is read to EOF and run as one chunk. Output goes to the client's
stdout/stderr (the run log). `$BOBLOX_SOCKET` is the only switch; omit it and no
listener starts.

## Options that matter

| flag | meaning |
| --- | --- |
| `--place-id N` | place to join |
| `--headless` | no GL (required on this box) |
| `--seconds N` | stop after N s; 0 = until Ctrl+C |
| `--exec FILE` | run FILE through the executor surface |
| `--no-scripts` | skip replicated game scripts (RAM saving) |
| `--roblosecurity C` | cookie on argv (visible in `ps`; prefer the file) |
| `--cookie-file F` | cookie from a file, e.g. `~/.config/boblox-cookie` |
| `--join-json F` | reuse a saved join reply |
| `--job-id UUID` | pin one running server instance |
| `--90-template/--8a-template/--pre-join` | join templates (default `run/`) |
| `--token-sub N` | force RUPP subtype (1 = join token) |

## Executor surface (inside `--exec`)

```lua
game.JobId game.PlaceId game.GameId
loadstring(src) getgenv() getrenv() getrawmetatable(t) setreadonly(t,false)
hookfunction(fn, hook) restorefunction(hook) newcclosure(fn)
game:HttpGet(url) request{Url=...,Method=...,Headers=...,Body=...}
readfile/writefile/isfile/listfiles (sandboxed to $ROBLOX_WORKSPACE)
crypt.hash/hmac/base64_encode/base64_decode
boblox.fire(remote, "OnServerEvent", {args})   -- args: string/number/bool/Instance
boblox.move(x, y, z)                            -- local Humanoid MoveDirection
boblox.send.ready/hold/control/release/raw
```

## Useful env knobs

| var | effect |
| --- | --- |
| `RBX_RAKNET_TXDUMP=dir` | dump our plaintext TX datagrams |
| `RBX_DUMP_JOIN90=path` | write our 0x90 json+body (`.json`/`.body`) |
| `RBX_DUMP_JOIN8A=path` | write our 0x8a body |
| `RBX_A6=1` | enable the legacy 0xA6 stream (off by default) |
| `RBX_TX_SUB=1\|2` | force RUPP subtype |
| `RBX_TX_TOKEN=0\|1\|2` | join token / session token / own token |
| `RBX_TLV2=peer\|rccpeerport` | override TLV2 (breaks Req1 with `peer`) |
| `RBX_NO_SCRIPTS` | not a knob; use `--no-scripts` |

## Read the result

```sh
... | grep -aE "world spawned|0x9B CHALLENGE|0x97 never|\[live\]|\[fire\]"
```

`world spawned=<n> ... engine=on` = the join worked.
`0x97 never arrived` = the server declined the join.

## Tests

```sh
cd build && ctest            # 20 tests
./tests/unit_batch           # encoder byte-exactness
```

## Native capture tooling (not needed to run the client)

```sh
cc -shared -fPIC -O2 -o tools/native_probe.so tools/native_probe.c -ldl
cc -O2 -o tools/nat_decode tools/nat_decode.c -lcrypto
BOBLOX_DRIVE_MODE=off ./tools/native_capture.sh <place> /tmp/nat.bin
./tools/nat_decode /tmp/nat.bin > /tmp/txplain.txt
```
