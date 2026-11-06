# Summary of the identity checks in play-build logs (docs/CODEGEN_SPEEDUP.md): one line per log.
#   python -I tools/perf/verify_summary.py <log> [...]
# Columns: codegen stack-access faults (stack_direct_access_verify), local_registers lockstep verify (calls / stores
# compared, mismatches), pdz_native_memcpy verify (memcpy / streaming calls checked, mismatches), record-thread
# constant differences (pdz_native_record_thread_verify), replay verify bad records / draw mismatches
# (pdz_native_record_workers_verify), draws issued and skipped at exit, and any crash / fatal line.
import re
import sys

for path in sys.argv[1:]:
    text = open(path, encoding="utf-8", errors="replace").read()
    stack = len(re.findall(r"codegen stack_direct_access_verify", text))
    lv = re.findall(r"local_registers verify: (\d+) calls of (\d+) functions verified, (\d+) calls / exits compared, "
                    r"(\d+) memory accesses matched \((\d+) stored bytes\), (\d+) mismatches", text)
    lv_bad = len(re.findall(r"local_registers verify: MISMATCH", text))
    lv_txt = "lockstep -" if not lv else (
        "lockstep %s fns, %s calls / exits, %s accesses, %s bad (%d logged)" % (lv[-1][1], lv[-1][2], lv[-1][3],
                                                                           lv[-1][5], lv_bad))
    mem = {}
    for kind, n, bad in re.findall(r"pdz_native_memcpy verify: (memcpy|streaming copy) sub_[0-9A-F]+: (\d+) calls checked \(\d+ MB\), (\d+) mismatches", text):
        mem[kind] = (int(n), int(bad))
    mem_txt = " ".join("%s %d/%d bad" % (k.split()[0], v[0], v[1]) for k, v in sorted(mem.items())) or "memcpy -"
    const_bad = len(re.findall(r"pdz_recthread: verify: .* differ from the device", text))
    rep = re.findall(r"verify (\d+) bad records, (\d+) draw count mismatches", text)
    rep_bad = sum(int(a) for a, _ in rep)
    rep_mis = sum(int(b) for _, b in rep)
    draws = re.findall(r"draws \(test exit\): issued (\d+) \| skipped: off (\d+) shader (\d+) targets (\d+) pipeline (\d+) geometry (\d+)", text)
    draws_txt = "draws -" if not draws else "draws %s skipped %d" % (draws[-1][0], sum(int(x) for x in draws[-1][1:]))
    crash = len(re.findall(r"(?i)unhandled exception|access violation|REX_FATAL|fatal error|crash dump", text))
    print("%-34s stack faults %d | %s | %s | constants differ %d | replay bad %d / draws %d | %s | crash lines %d" % (
        path.replace("\\", "/").split("/")[-1], stack, lv_txt, mem_txt, const_bad, rep_bad, rep_mis, draws_txt, crash))
