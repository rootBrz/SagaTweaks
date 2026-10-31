# Which code rolls the game's random function (sub_54FA60) and how often per frame.
# Run with the game idle next to the object in question:
#   gdb -q -batch -p $(pgrep LEGO | head -1) -x tools/audit/rand_callers.gdb   (send SIGINT after ~5 s: kill -INT <pid>)
# then:  ... | grep -E '^(C|F)' | sort | uniq -c | sort -rn
# "C <addr>" is the return address of a call (call site = addr - 5), "F" one game update (sub_491D50). A caller with about
# as many C lines as F lines rolls once per frame: look at what its roll gates (docs/CLASSES.md, "per-frame rolls").
set pagination off
set confirm off
break *0x54FA60
commands 1
silent
printf "C %x\n",*(int*)$esp
continue
end
break *0x491D50
commands 2
silent
printf "F\n"
continue
end
continue
