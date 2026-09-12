#!/bin/sh
# Print the ssh target for the build machine.
#
# On the home LAN it answers directly at 192.168.1.21, which is much faster;
# from anywhere else only the cloudflared tunnel behind the "arch" alias works.
# Probe the fast path briefly and fall back, so no script has to care.
#
#   HOST=$(sh "$(dirname "$0")/buildhost.sh")
#
# BUILD_HOST overrides the probe entirely.
if [ -n "$BUILD_HOST" ]; then
    echo "$BUILD_HOST"
    exit 0
fi
LAN=${BUILD_HOST_LAN:-sebastian@192.168.1.21}
TUNNEL=${BUILD_HOST_TUNNEL:-arch}
if ssh -o BatchMode=yes -o ConnectTimeout=4 -o StrictHostKeyChecking=accept-new "$LAN" true 2>/dev/null; then
    echo "$LAN"
else
    echo "$TUNNEL"
fi
