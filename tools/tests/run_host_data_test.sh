#!/bin/sh
# Fixture test for tools/host_data.py: convert, assemble -m32, link, run.
set -e
cd "$(dirname "$0")/../.."
rm -rf build/host/data_test
mkdir -p build/host/data_test
python3 tools/host_data.py tools/tests build/host/data_test
g++ -m32 -c build/host/data_test/host_data_rodata.host.s -o build/host/data_test/rodata.o
g++ -m32 -c build/host/data_test/host_data_vtable.host.s -o build/host/data_test/vtable.o
g++ -m32 -malign-double -std=gnu++26 -Iinclude -Iinclude/host tools/tests/host_data_consumer.cpp \
    build/host/data_test/rodata.o build/host/data_test/vtable.o -o build/host/data_test/consumer
./build/host/data_test/consumer
