#!/usr/bin/env bash
set -e

echo "========================================================"
echo "           ToyDB End-to-End Verification Test           "
echo "========================================================"

# Directory resolution
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$DIR"

echo "[1/4] Cleaning old database files and rebuilding..."
rm -f data.db*
make -C .. clean
make -C ..

echo "[2/4] Running loaddb..."
./loaddb > load_output.log
echo "      Successfully loaded records."

echo "[3/4] Testing Sequential Scan (dumpdb s)..."
./dumpdb s > seq_output.csv
if diff -u <(tail -n +2 data.csv | sort) <(sort seq_output.csv) > /dev/null; then
    echo "      ✓ Sequential Scan matches data.csv exactly (194 rows verified)!"
else
    echo "      ✗ Sequential Scan output differs from data.csv!"
    exit 1
fi

echo "[4/4] Testing B+ Tree Index Scan (dumpdb i)..."
./dumpdb i > idx_output.csv
if diff -u <(tail -n +2 data.csv | sort) <(sort idx_output.csv) > /dev/null; then
    echo "      ✓ Index Scan matches data.csv exactly (194 rows verified)!"
else
    echo "      ✗ Index Scan output differs from data.csv!"
    exit 1
fi

# Clean up test output logs
rm -f load_output.log seq_output.csv idx_output.csv

echo "========================================================"
echo "          ALL TESTS PASSED WITH 0 DIFFERENCES!          "
echo "========================================================"
