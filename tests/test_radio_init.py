"""Exercise native init/TX bodies with driver failures, without radio hardware."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/Radio.cpp").read_text()
header = (ROOT / "src/Radio.h").read_text()


def extract(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


code = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "RadioSettings.h"
using byte = uint8_t;
constexpr int RADIOLIB_ERR_NONE = 0, RADIOLIB_ERR_UNKNOWN = -1;
constexpr int RADIOLIB_ERR_PACKET_TOO_LONG = -4;
constexpr int GPIO_NUM_7 = 7, GPIO_NUM_2 = 2, GPIO_NUM_5 = 5;
constexpr int OUTPUT = 1, HIGH = 1, LOW = 0;
constexpr uint32_t RADIOLIB_NC = 0xFFFFFFFF;
#define END_OF_MODE_TABLE {0, {0}}
#define info(...) ((void)0)
#define error(...) ((void)0)
void pinMode(int, int) {}
void digitalWrite(int, int) {}
struct { void begin(int, int, int, int) {} } SPI;
constexpr int RADIO_SCK=5, RADIO_MISO=19, RADIO_MOSI=27, RADIO_NSS=18;
struct Module {
    static constexpr int RFSWITCH_MAX_PINS = 5;
    enum { MODE_IDLE, MODE_RX, MODE_TX };
    struct RfSwitchMode_t { int mode; int values[5]; };
};
struct Driver {
    int fail = 0, calls = 0, sent = 0, powerCalls = 0;
    int power = 10; bool optimized = true;
    std::vector<int> order;
    int step(int id) { ++calls; order.push_back(id); return fail == id ? -100-id : 0; }
    int beginFSK() { power=10; optimized=true; return step(1); }
    int setOutputPower(int p, bool optimize) {
        ++powerCalls; power=p; optimized=optimize; return step(2);
    }
    int setFrequency(float value) { assert(value==868.96f); return step(3); }
    int setBitRate(float value) { assert(value==25.0f); return step(4); }
    int setFrequencyDeviation(float value) { assert(value==50.0f); return step(5); }
    int setRxBandwidth(float value) { assert(RadioSettings::validRxBandwidth(value)); return step(6); }
    int setPreambleLength(int value) { assert(value==16); return step(7); }
    void setRfSwitchTable(const uint32_t*, const Module::RfSwitchMode_t*) {}
    void setRfSwitchPins(int, int) {}
    int standby() { return step(8); }
    int setCRC(bool, bool) { return step(9); }
    int setCRC(int, int, int, bool) { return step(9); }
    int variablePacketLengthMode(size_t) { return step(10); }
    int transmit(byte*, size_t) { ++sent; return step(11); }
};
class Radio {
public:
    static constexpr size_t MAX_RX_PACKET_LENGTH=63;
    Driver _radio;
    void init(float rxBandwidth=RadioSettings::DEFAULT_RX_BANDWIDTH,
              bool lnaBypass=RadioSettings::DEFAULT_LNA_BYPASS);
    int16_t transmit(byte*, size_t);
private:
''' + "\n".join(re.findall(r"^\s*int16_t _initResult[^;]*;", header, re.M)) + r'''
};
'''
profile = source[source.index("#if defined(WIFI_LORA_32_V4) && defined(USE_KCT8103L_PA)"):source.index("Radio::Radio()")]
code += profile + extract("void Radio::init(") + "\n" + extract("int16_t Radio::transmit(")
code += r'''
int main() {
    byte data[10] = {};
    Radio radio;
    assert(radio.transmit(data, sizeof(data)) == RADIOLIB_ERR_UNKNOWN);
    assert(radio._radio.calls == 0 && radio._radio.sent == 0);
    const bool kct =
#if defined(WIFI_LORA_32_V4) && defined(USE_KCT8103L_PA)
        true;
#else
        false;
#endif
    for (int failure = 1; failure <= 7; ++failure) {
        if (failure == 2 && !kct) continue;
        radio._radio.fail = failure;
        radio.init();
        const int calls = radio._radio.calls;
        assert(radio.transmit(data, sizeof(data)) == -100-failure);
        assert(radio._radio.sent == 0 && radio._radio.calls == calls);
        assert(radio._radio.order.back() == failure);
        // Recovery is a fresh native init, never a fallback to beginFSK's power.
        radio._radio.fail = 0;
        radio.init(0.0f, false); // invalid bandwidth uses the native default
        assert(radio.transmit(data, sizeof(data)) == 0);
        assert(radio._radio.sent == 1);
        assert(radio._radio.power == (kct ? -9 : 10));
        assert(radio._radio.optimized == !kct);
        radio._radio.sent = 0;
    }
    radio.init(RadioSettings::DEFAULT_RX_BANDWIDTH, true);
    assert(radio.transmit(data, sizeof(data)) == 0);
    for (int failure = 8; failure <= 10; ++failure) {
        radio._radio.fail = failure;
        int sent = radio._radio.sent;
        assert(radio.transmit(data, sizeof(data)) == -100-failure);
        assert(radio._radio.sent == sent);
    }
    assert(kct || radio._radio.powerCalls == 0);
    puts("OK: before-init, all init failures, reinit/recovery, native PA and TX error propagation");
}
'''
with tempfile.TemporaryDirectory(prefix="frisquet-radio-init-") as directory:
    cpp = Path(directory) / "test.cpp"
    cpp.write_text(code)
    for name, flags in (
        ("v43-kct", ["-DWIFI_LORA_32_V4", "-DUSE_KCT8103L_PA"]),
        ("v3", []),
        ("sx1276", ["-DRADIO_SX1276"]),
    ):
        binary = Path(directory) / name
        subprocess.run(["c++", "-std=c++11", "-fsanitize=address,undefined",
                        *flags, "-I", str(ROOT / "src"), str(cpp), "-o", str(binary)], check=True)
        print(name, flush=True)
        subprocess.run([str(binary)], check=True)
