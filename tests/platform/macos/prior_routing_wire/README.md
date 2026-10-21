# Pre-negotiation wire fixtures

These four files preserve the routing connection/broker implementation before
sample-rate capability negotiation. Tests compile the original strict handshake
and four-field pair decoder under separate class names, alongside the current
implementation. Keep these snapshots unchanged when evolving the protocol.

They exercise the old wire behavior through real authenticated private XPC. They
do not reproduce an older HAL binary or certify a deployed mixed-version setup.
