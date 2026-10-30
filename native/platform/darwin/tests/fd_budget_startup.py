"""Run the actual host binary with a GUI-sized inherited descriptor budget."""
import os
import re
import resource
import subprocess
import sys

hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
if hard != resource.RLIM_INFINITY and hard < 256:
    sys.exit(77)
kernel_cap = int(subprocess.check_output(["/usr/sbin/sysctl", "-n", "kern.maxfilesperproc"]))
expected = min(4096, kernel_cap, hard if hard != resource.RLIM_INFINITY else 4096)


def low_limit():
    resource.setrlimit(resource.RLIMIT_NOFILE, (256, hard))


env = os.environ | {"DYLD_INSERT_LIBRARIES": sys.argv[2]}
env.pop("RAVEN_TRACE_FILE", None)
result = subprocess.run([sys.argv[1]], env=env, preexec_fn=low_limit, capture_output=True, text=True, timeout=15)
match = re.search(r"^fd-budget-observed (\d+) (\d+)$", result.stdout, re.MULTILINE)
assert match, (result.returncode, result.stdout, result.stderr)
soft, after_hard = map(int, match.groups())
assert soft == max(256, expected) and after_hard == hard, (soft, after_hard, expected, hard)
print(f"built raven startup raised soft limit 256 -> {soft}; hard limit unchanged")
