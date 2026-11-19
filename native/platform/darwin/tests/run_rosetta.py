"""Skip x86_64 fixtures on hosts without Rosetta before attempting execution."""
import subprocess
import sys


def main(command):
    try:
        probe = subprocess.run(["/usr/bin/arch", "-x86_64", "/usr/bin/true"], capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        print("Rosetta execution unavailable")
        return 77
    if probe.returncode:
        print("Rosetta execution unavailable")
        return 77
    return subprocess.call(command)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
