#!/usr/bin/env sh
# Build and run the native (host) unit tests for GoogleSchedular.
# Compiled as gnu++11 to mirror the AVR/ESP core (also guards the odr-use fix).
#
# The tests compile the real, unmodified library against the mocks in
# test/mock/, plus the real portable dependencies (ArduinoJson, FastTimer's
# TimestampNtp). ESP8266 selects the library's include branch; ARDUINO makes
# ArduinoJson use its Arduino String/Stream adapters (fed by our mocks), while
# its Print adapter is disabled since the tests never serialize through Print.
#
# Paths to the two external, header-only dependencies. Override via the
# environment to point at a different checkout/install.
set -e
here="$(cd "$(dirname "$0")" && pwd)"

FASTTIMER_SRC="${FASTTIMER_SRC:-$here/../../Arduino-FastTimer/src}"
ARDUINOJSON_SRC="${ARDUINOJSON_SRC:-$HOME/Documents/Arduino/libraries/ArduinoJson/src}"

if [ ! -f "$FASTTIMER_SRC/TimestampNtp.hpp" ]; then
    echo "error: TimestampNtp.hpp not found under FASTTIMER_SRC=$FASTTIMER_SRC" >&2
    echo "       set FASTTIMER_SRC to the Arduino-FastTimer src/ directory." >&2
    exit 2
fi
if [ ! -f "$ARDUINOJSON_SRC/ArduinoJson.h" ]; then
    echo "error: ArduinoJson.h not found under ARDUINOJSON_SRC=$ARDUINOJSON_SRC" >&2
    echo "       set ARDUINOJSON_SRC to the ArduinoJson src/ directory." >&2
    exit 2
fi

out="$(mktemp -d)/googleschedular_tests"

# -Werror so warnings in the library or the tests fail the native-test job.
# ArduinoJson is a third-party dependency included with -isystem so its own
# headers do not trip -Werror; the mocks, the library and the tests are held to
# -Wall -Wextra -Werror.
${CXX:-c++} -std=gnu++11 -Wall -Wextra -Werror \
    -DARDUINO=10805 -DESP8266=1 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 \
    -I "$here/mock" -I "$here/../src" -I "$FASTTIMER_SRC" \
    -isystem "$ARDUINOJSON_SRC" \
    "$here/test_main.cpp" -o "$out"

exec "$out"
