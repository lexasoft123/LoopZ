"""The native metadata worker accepts bounded, sequenced read-only requests."""

import json
import subprocess
import sys

checker = sys.argv[1]
for request in (
    "unknown\n",
    "snapshot 0\n",
    "snapshot -1\n",
    "snapshot 1 extra\n",
    "x" * 80 + "\n",
    "snapshot 1",
):
    result = subprocess.run(
        [checker, "--snapshot-server"], input=request, capture_output=True, text=True, timeout=5
    )
    assert result.returncode == 2, (request, result.stdout, result.stderr)
    assert not result.stdout
result = subprocess.run(
    [checker, "--snapshot-server"],
    input="snapshot 41\nsnapshot 42\nquit\n",
    capture_output=True,
    text=True,
    timeout=40,
)
assert result.returncode == 0, (result.stdout, result.stderr)
replies = [json.loads(line) for line in result.stdout.splitlines()]
assert [reply["request"] for reply in replies] == [41, 42]
for reply in replies:
    assert set(reply) in ({"request", "catalog"}, {"request", "error"})
    if "catalog" in reply:
        assert isinstance(reply["catalog"]["routingDevices"], list)
        assert isinstance(reply["catalog"]["defaultOutput"], str)
    else:
        assert isinstance(reply["error"], str) and reply["error"]
print("Native catalog worker validates requests and serves repeated sequenced replies")
