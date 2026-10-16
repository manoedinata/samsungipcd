#!/usr/bin/env python3
"""Checks the SIM related AT commands (AT+CPIN?, AT+CIMI, AT+CCID, AT+CRSM, AT+SIMSTATE?, AT+CMEE)
against the simulator for every SIM state and every error format.

    make samsungipcd-sim && python3 tests/sim_at.py ./samsungipcd-sim

The simulator picks the SIM state from the PHONESIM_SIM environment variable
(the MODEM_SIM_* numbers of modem.h); the script drives it through a pty."""
import os, pty, tty, subprocess, time, select, sys
SIM = sys.argv[1] if len(sys.argv) > 1 else "./samsungipcd-sim"
# MODEM_SIM_* values in modem.h order
S = dict(UNKNOWN=0, ABSENT=1, ERROR=2, BLOCKED=3, READY=4, PIN=5, PUK=6, NET=7, NETSUB=8, SP=9, CORP=10)
CPIN = {"READY":"+CPIN: READY","PIN":"+CPIN: SIM PIN","PUK":"+CPIN: SIM PUK","NET":"+CPIN: PH-NET PIN",
        "NETSUB":"+CPIN: PH-NETSUB PIN","SP":"+CPIN: PH-SP PIN","CORP":"+CPIN: PH-CORP PIN"}
ERRN = {"ABSENT":(10,"SIM not inserted"),"ERROR":(13,"SIM failure"),"BLOCKED":(13,"SIM failure"),"UNKNOWN":(14,"SIM busy")}

def ask(state, cmee, cmd):
    master, slave = pty.openpty()
    tty.setraw(slave)
    env = dict(os.environ, PHONESIM_SIM=str(S[state]))
    p = subprocess.Popen([SIM, os.ttyname(slave)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    def xfer(c):
        os.write(master, (c + "\r").encode()); out = b""; end = time.time() + 1.5
        while time.time() < end:
            if select.select([master], [], [], 0.1)[0]:
                out += os.read(master, 4096)
                if out.rstrip().endswith((b"OK", b"ERROR")) or b"+CME ERROR" in out: break
        return out.decode().replace("\r", "").strip("\n").replace("\n\n", "\n").split("\n")
    xfer("AT"); xfer("AT+CMEE=%d" % cmee)
    r = xfer(cmd)
    p.kill(); p.wait(); os.close(master); os.close(slave)
    return [x for x in r if x]

fails = 0
def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "\n       got  %r\n       want %r" % (got, want)))

for st in S:
    for cmee in (0, 1, 2):
        got = ask(st, cmee, "AT+CPIN?")
        if st in CPIN: want = [CPIN[st], "OK"]
        else:
            n, t = ERRN[st]
            want = ["ERROR"] if cmee == 0 else (["+CME ERROR: %d" % n] if cmee == 1 else ["+CME ERROR: %s" % t])
        check("CPIN? state=%-7s cmee=%d" % (st, cmee), got, want)
for st in ("READY", "ABSENT", "UNKNOWN", "PIN"):
    got = ask(st, 1, "AT+CIMI")
    if st == "READY": check("CIMI  state=READY   (IMSI present, 15 digits)", [len(got[0]) == 15 and got[0].isdigit(), got[1]], [True, "OK"])
    else: check("CIMI  state=%-7s cmee=1" % st, got, ["+CME ERROR: %d" % (10 if st == "ABSENT" else 14)])
check("SIMSTATE? READY ", ask("READY", 0, "AT+SIMSTATE?"), ["+SIMSTATE: 1", "OK"])
check("SIMSTATE? ABSENT", ask("ABSENT", 0, "AT+SIMSTATE?"), ["+SIMSTATE: 0", "OK"])
check("SIMSTATE? PIN   ", ask("PIN", 0, "AT+SIMSTATE?"), ["+SIMSTATE: 0", "OK"])
check("CCID  READY (digits, F padding dropped)", ask("READY", 0, "AT+CCID"), ["+CCID: 8900000000000000000", "OK"])
check("ICCID READY", ask("READY", 0, "AT+ICCID"), ["+CCID: 8900000000000000000", "OK"])
check("CRSM  read EF_ICCID", ask("READY", 0, "AT+CRSM=176,12258,0,0,10"), ['+CRSM: 144,0,"%s"' % ("98" + "00" * 8 + "F0"), "OK"])
check("CRSM  other file is not available", ask("READY", 0, "AT+CRSM=176,28474,0,0,10"), ["ERROR"])
check("CCID  ABSENT cmee=1", ask("ABSENT", 1, "AT+CCID"), ["+CME ERROR: 10"])
check("CCID  UNKNOWN cmee=1", ask("UNKNOWN", 1, "AT+CCID"), ["+CME ERROR: 14"])
check("CRSM  ABSENT cmee=0", ask("ABSENT", 0, "AT+CRSM=176,12258,0,0,10"), ["ERROR"])
print("\n%s" % ("ALL PASSED" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
