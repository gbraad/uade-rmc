#!/bin/bash

RMC="./rmc"

echo "Test that unpack && pack yields the original container"
set -e
"${RMC}" test-songs/dlm2.ion-cannon4 2>/dev/null
rm -rf test-pack-dir
mkdir test-pack-dir
"${RMC}" -u test-pack-dir test-songs/dlm2.ion-cannon4.rmc 2>/dev/null
"${RMC}" -p test-pack-dir test.rmc 2>/dev/null
if ! cmp test-songs/dlm2.ion-cannon4.rmc test.rmc ; then
    echo "Error: Files are different"
    exit 1
fi
