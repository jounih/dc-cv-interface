#!/bin/sh
# Run every check; full output goes to logs/, only a summary is printed.
#   ./check.sh                 (ESP-IDF builds run when ~/esp/esp-idf or $IDF_PATH exists)
cd "$(dirname "$0")" || exit 1
mkdir -p logs
fail=0
step() {  # name, command...
    name=$1; shift
    if "$@" > "logs/$name.log" 2>&1; then echo "ok    $name"; else echo "FAIL  $name (logs/$name.log)"; fail=1; fi
}
step host-tests   make -C firmware/test
step cvcal        python3 tools/cvcal.py selftest
step spice        python3 hw/spice/run_spice.py
PY=python3; [ -x .venv/bin/python ] && PY=.venv/bin/python
step hw-gen       $PY hw/gen_hw.py
IDF=${IDF_PATH:-$HOME/esp/esp-idf}
if [ -f "$IDF/export.sh" ]; then
    step fw-esp32s3 sh -c ". '$IDF/export.sh' >/dev/null 2>&1 && cd firmware/esp32s3 && idf.py build"
    step fw-tiera   sh -c ". '$IDF/export.sh' >/dev/null 2>&1 && cd firmware/esp32s3 && idf.py -B build-tiera -DCV_TIER_A=1 -DSDKCONFIG=build-tiera/sdkconfig build"
else
    echo "skip  firmware builds (ESP-IDF not found)"
fi
grep -h "tests:\|spice:\|netlist:\|bom full" logs/host-tests.log logs/spice.log logs/hw-gen.log 2>/dev/null
exit $fail
