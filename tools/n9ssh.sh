#!/bin/sh
# SSH to the Nokia N9 (Harmattan, OpenSSH 5.1p1).
#
# That server predates ed25519 and SHA-2 signatures, so it needs the old
# ssh-rsa algorithm both for its host key and for our key, and a key of its own
# (~/.ssh/id_rsa_n9) -- the regular ed25519 key is unknown to it.
#
#   tools/n9ssh.sh                 interactive shell
#   tools/n9ssh.sh 'command'       run a command
#   N9_HOST=... N9_USER=...        override the target
exec ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa \
    -i "$HOME/.ssh/id_rsa_n9" -oConnectTimeout=10 \
    "${N9_USER:-user}@${N9_HOST:-192.168.1.12}" "$@"
