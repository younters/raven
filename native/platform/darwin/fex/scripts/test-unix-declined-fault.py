import subprocess
import sys
for mode in ('--unix-host-fault','--unix-dispatch-fault'):
    result=subprocess.run([sys.argv[1],mode],capture_output=True,text=True,timeout=10)
    expected=138 if mode=='--unix-dispatch-fault' else 139
    if result.returncode!=expected or 'unhandled FEX native or unsupported fault' not in result.stderr:
        sys.exit(f'{mode}: expected diagnostic exit {expected}, got {result.returncode}: {result.stderr}')
subprocess.run([sys.argv[1],'--unix-explicit-fault'],check=True,timeout=10)
print('PASS: unrelated host and nested exception-dispatch faults cannot unwind unix guard')
