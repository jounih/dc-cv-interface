#!/bin/sh
# Run every check; full output goes to logs/, only a summary is printed.
#   PICO_SDK_PATH=~/.pico-sdk/sdk PICO_TOOLCHAIN_PATH=~/.pico-sdk/arm-gnu-toolchain-... ./check.sh
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
if [ -n "$PICO_SDK_PATH" ]; then
    [ -n "$PICO_TOOLCHAIN_PATH" ] && PATH="$PICO_TOOLCHAIN_PATH/bin:$PATH"
    step fw-pico2   sh -c 'cmake -S firmware -B firmware/build -DPICO_BOARD=pico2 && cmake --build firmware/build -j8'
    step fw-pico2-8 sh -c 'cmake -S firmware -B firmware/build8 -DPICO_BOARD=pico2 -DCV_N_OUT=8 && cmake --build firmware/build8 -j8'
else
    echo "skip  firmware build (set PICO_SDK_PATH)"
fi
grep -h "host tests\|spice:\|netlist:\|bom:" logs/*.log
exit $fail
