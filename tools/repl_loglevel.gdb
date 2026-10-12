# repl_loglevel.gdb — force DFLog::ReplicationDataCaching verbose on a live
# mocktail engine so the join path logs instance boundaries:
#   "Expecting %d instances", "Reading instance @ offset=%d",
#   "Begin reading join instance", "Done reading join instance %s ...".
# The gate is (u8)chan >= 6 && BYTE1(chan) >= 3 at file off 0x75E3F78.
# Must run BEFORE the snapshot arrives (~3s after launch).
set pagination off
set confirm off
python
import gdb
pid = gdb.selected_inferior().pid
base = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        base = int(line.split("-")[0], 16)
        break
assert base is not None, "libroblox base not found"
chan = base + 0x75E3F78
gdb.execute(f"set *(char*){chan} = 6", to_string=True)
gdb.execute(f"set *(char*)({chan}+1) = 6", to_string=True)
print(f"ReplicationDataCaching verbose forced at {hex(chan)}")
end
detach
quit
