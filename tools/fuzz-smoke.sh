#!/usr/bin/env bash
# Fuzz harness entry point. Real libFuzzer targets arrive with F1.
echo "fuzz: no targets yet (F1 delivers libFuzzer harnesses); duration requested: ${1:-60}s"
exit 0
