# Executor surface

The lean UNC subset installed into a live Boblox Engine state. Boblox owns its
Luau VM, so this is a **registration**, not an injection: no `LD_PRELOAD`, no
offsets, no build-id pin. The client calls `rbx::exec::install()` right after
`engine::attach()`, and `--exec FILE` runs one user script through the same
scheduler as replicated scripts.

Source: `client/src/exec/exec.{h,cpp}` · tests: `tests/unit/unit_exec.cpp` ·
demo: `run/exec_demo.lua`.

## Backed

| area | names |
| --- | --- |
| environment | `getgenv` `getrenv` `getreg` `getfenv` `setfenv` `getthreadidentity` `setthreadidentity` `checkcaller` `getgc` |
| identity | `identifyexecutor` `getexecutorname` `isexecutorclosure` |
| code | `loadstring` `loadfile` |
| closures | `newcclosure` `isnewcclosure` `clonefunction` `hookfunction` `restorefunction` `isfunctionhooked` |
| metatables | `getrawmetatable` `setrawmetatable` `hookmetamethod` `setreadonly` `isreadonly` `makereadonly` |
| http | `request` `http_request` `syn.request` `http.request` `fluxus.request` `game:HttpGet(Async)` `game:HttpPost(Async)` |
| filesystem | `readfile` `writefile` `appendfile` `isfile` `isfolder` `makefolder` `delfile` `delfolder` `listfiles` `getcustomasset` |
| crypto | `crypt.base64_encode` `crypt.base64_decode` `crypt.hash` `crypt.hmac` `crypt.generatebytes` |
| misc | `setclipboard` `getclipboard` `isrbxactive` `messagebox` `queue_on_teleport` `clearqueueonteleport` |
| upstream | `boblox.send.ready/hold/control/release/raw` (see `SEND.md`) |

### Semantics that differ from a native executor

- **`hookfunction`** swaps the function value in `_G` and `game` (the tables a
  script can actually reach) and returns the original. It does not retarget
  arbitrary closure objects — the stock Luau C API cannot rewrite a closure's
  proto pointer. `restorefunction` undoes the swap.
- **`newcclosure` / `clonefunction`** return the function itself and record it
  so `isnewcclosure` / `isexecutorclosure` answer correctly. There is no C
  closure wrapping to hide.
- **`getgc`** is a bounded reachability walk from `_G` and `game` (depth 12,
  20k nodes), not a heap scan: the VM exposes no GC traversal API.
- **filesystem** is sandboxed to `$BOBLOX_WORKSPACE`
  (default `$HOME/.boblox/workspace`); `..` is rejected.
- **identity** is always 8 (`getthreadidentity`).

## Not backed (deliberate)

These need Roblox's native internals or a working send path, neither of which
this client has. They are absent rather than stubbed, so a script fails loudly
instead of silently behaving differently:

- signal introspection — `getconnections`, `firesignal`, `getsignalarguments`
- raw VM — `getconstants`, `getprotos`, `getscriptbytecode`, `decompile`
- reflection — `getcallbackvalue`, property descriptors, vulnpatch
- remotes — `firetouchinterest`, `fireproximityprompt`, `FireServer`

`firetouchinterest` / `fireproximityprompt` are registered as no-ops so scripts
that unconditionally call them keep running; they do nothing until the send
path lands (see `FINDINGS.md`, "Send path").

## Build

```sh
source ~/work/env.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DNLOHMANN_JSON_INCLUDE_DIR=$HOME/work/Rum/third_party/nlohmann_json/include \
  -DRBX_VIEWER=OFF
cmake --build build -j --target roblox unit_exec
./build/tests/unit_exec
```
