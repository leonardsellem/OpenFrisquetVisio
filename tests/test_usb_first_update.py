"""Run real App lifecycle/update-route code with native service/HTTP stubs.

Run: python3 tests/test_usb_first_update.py
Full login/session behavior is covered by test_portal_auth.py.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
app = (ROOT / "src/App.cpp").read_text()
portal = (ROOT / "src/Portal.cpp").read_text()


def extract(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


route_match = re.search(r'^  onAuthenticated\("/api/update"[^\n]+', portal, re.M)
assert route_match, "Authenticated HTTP firmware-upload route must remain registered"
route = route_match[0]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <functional>
unsigned otaStarted=0, otaServiced=0, portalLoops=0, networkLoops=0;
unsigned mqttLoops=0, boilerLoops=0;
struct Service { unsigned& calls; void begin(){} void loop(){++calls;} };
struct Ota { void loop(){++otaServiced;} };
struct { void begin(int){} } Serial;
struct { void begin(bool,bool,bool){} } Heltec;
#define BOARD_HELTEC
#define WIFI_AP_STA 3
struct { bool connected=true; bool isConnected(){return connected;} void mode(int){} } WiFi;
uint32_t millis(){return 100;}
void delay(int){}
void info(const char*,...){}
class App { public:
    Service _networkManager{networkLoops}, _mqtt{mqttLoops}, _frisquetManager{boilerLoops};
    Service portalService{portalLoops}; Service* _portal=&portalService;
    Ota _ota; bool _apFallbackArmed=false; uint32_t _apFallbackAtMs=0;
    void initConfig(){} void initNetwork(){} void initMqtt(){} void initPortal(){}
    void initOta(){++otaStarted;}
    void begin(); void loop();
};
enum HTTPMethod { HTTP_POST };
struct WebServer {
    std::function<void()> handler, upload;
    template<class F,class G> void on(const char*,HTTPMethod,F f,G g){handler=f;upload=g;}
    template<class F> void on(const char*,HTTPMethod,F f){handler=f;}
};
class Portal { public:
    WebServer _srv; bool authenticated=false; unsigned finalized=0, written=0, rejected=0;
    bool isAuthenticated(){return authenticated;}
    bool requireAuthentication(){if(!authenticated)++rejected;return authenticated;}
    void handleUpdate(){++finalized;} void handleUpdateUpload(){++written;}
    void onAuthenticated(const char*,HTTPMethod,void(Portal::*)(),void(Portal::*)());
    void begin(){
''' + route + r'''
    }
};
'''
code += extract(app, "void App::begin(") + "\n" + extract(app, "void App::loop(")
code += "\n" + extract(portal, "void Portal::onAuthenticated(")
code += r'''
int main(){
    App app;app.begin();app.loop();
    WiFi.connected=false;app._apFallbackArmed=true;app.loop();
    assert(portalLoops==2 && networkLoops==2 && mqttLoops==2 && boilerLoops==2);
    Portal portal;portal.begin();assert(portal._srv.handler && portal._srv.upload);
    for(int i=0;i<4;++i)portal._srv.upload();portal._srv.handler();
    assert(portal.written==0 && portal.finalized==0 && portal.rejected==1);
    portal.authenticated=true;
    for(int i=0;i<4;++i)portal._srv.upload();portal._srv.handler();
    assert(portal.written==4 && portal.finalized==1);
    fprintf(stderr,"ArduinoOTA lifecycle: starts=%u, services=%u; HTTP: denied without auth, allowed with auth\n",otaStarted,otaServiced);
    assert(otaStarted==0 && otaServiced==0);
}
'''
env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": "/home/ubuntu",
       "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
       "TMPDIR": os.environ.get("TMPDIR", "/home/ubuntu/.hermes/profiles/cole/cache/scratch")}
with tempfile.TemporaryDirectory(prefix="frisquet-usb-first-", dir=env["TMPDIR"]) as temp:
    cpp = Path(temp) / "test.cpp"
    cpp.write_text(code)
    binary = Path(temp) / "test"
    subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", "-fsanitize=address,undefined",
                    str(cpp), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)

assert not (ROOT / "src/OTA.h").exists(), "Dead ArduinoOTA helper must be deleted"
for source in (ROOT / "src").rglob("*"):
    if source.suffix in (".h", ".cpp"):
        assert not re.search(r'\bArduinoOTA\b|\binitOta\b|\b_ota\b|"OTA\.h"', source.read_text()), source
print("OK: no ArduinoOTA startup/service/helper; authenticated HTTP upload retained")
