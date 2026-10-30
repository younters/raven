"""Check event-handoff benchmark output and process status independently."""
import re
import subprocess
import sys


def main(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=25)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    if result.returncode:
        return 1
    if not re.search(r'^2000 [0-9]+ [0-9]+$', result.stdout, re.MULTILINE):
        print('Missing event-handoff measurements', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
