"""Real application methods, native service/chip doubles; NOT RF/thermal evidence.

Run all: python3 tests/test_commissioning.py
Native groups cover pairing, observations, parsing, lease and persisted RF admission.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / 'src' / path).read_text()


def stripped(path):
    return re.sub(r'^#(?:include|pragma)[^\n]*\n', '', source(path), flags=re.M)


def extract(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


prefix = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>
using byte=uint8_t;
using std::isnan;
using std::isfinite;
uint32_t tick=100;
uint32_t millis(){return tick;}
void delay(unsigned n){tick+=n;}
time_t clockNow=1793224800;
bool clockValid=true;
enum {timeNotSet,timeSet};
int timeStatus(){return clockValid?timeSet:timeNotSet;}
time_t now(){return clockNow;}
unsigned year(time_t t){return gmtime(&t)->tm_year+1900;}
unsigned weekday(time_t t){return gmtime(&t)->tm_wday+1;}
unsigned hour(time_t t){return gmtime(&t)->tm_hour;}
unsigned minute(time_t t){return gmtime(&t)->tm_min;}
unsigned second(time_t t){return gmtime(&t)->tm_sec;}
void setTime(int,int,int,int,int,int){}
void info(const char*,...){} void error(const char*,...){} void debug(const char*,...){}
struct String:std::string {
    using std::string::string;
    String(const std::string& s):std::string(s){}
    String(unsigned n):std::string(std::to_string(n)){}
    String(int n):std::string(std::to_string(n)){}
    String(float n):std::string(std::to_string(n)){}
    String(double n):std::string(std::to_string(n)){}
    bool equalsIgnoreCase(const char* s)const{return !strcasecmp(c_str(),s);}
    float toFloat()const{return atof(c_str());}
};
void warning(const String&){}
struct MqttTopic {String full;MqttTopic(){} MqttTopic(String s,int=0,bool=false):full(s){}};
struct MqttEntity {
    String id,name,component,value;MqttTopic stateTopic,commandTopic,availabilityTopic;
    std::map<String,String> extraFields;
    template<class T> void set(const String& k,const T& v){extraFields[k]=String(v);}
    void setRaw(const String& k,const String& v){extraFields[k]=v;}
};
struct MqttDevice {
    String baseTopic="frisquet";
    std::map<String,MqttEntity*> entities;
    MqttEntity* getEntity(const String& s){return entities.at(s);}
};
struct MqttManager {
    MqttDevice device;bool online=true;uint32_t generation=1;
    std::map<String,std::function<void(const String&)>> handlers;
    bool connected(){return online;}
    uint32_t connectionGeneration()const{return generation;}
    String controlSession()const{return String(generation);}
    void registerEntity(MqttDevice& d,MqttEntity& e,bool){d.entities[e.id]=&e;}
    void onCommand(const MqttEntity& e,std::function<void(const String&)> f){handlers[e.commandTopic.full]=f;}
    void onCommand(const MqttTopic& t,std::function<void(const String&)> f){handlers[t.full]=f;}
    template<class T> void publishState(MqttEntity& e,T v){e.value=String(v);}
    void publishAvailability(MqttEntity&,bool){}
    MqttDevice* getDevice(const String&){return &device;}
    static String compose(std::initializer_list<String> parts){String r;for(auto& p:parts){if(r.length())r+="/";r+=p;}return r;}
    void command(const char* field,const String& value){handlers.at(String("frisquet/z1/")+field+"/set")(value);}
};
struct Preferences {
    static std::map<std::string,std::vector<byte>> saved;
    static unsigned writes,failAt;
    String space;
    bool begin(const String& name,bool){space=name;return true;} void end(){}
    bool isKey(const char*){return true;}
    float getFloat(const char*,float d=0){return d;}
    uint8_t getUChar(const char*,uint8_t d=0){return d;}
    unsigned getUInt(const char*,unsigned d=0){return d;}
    bool getBool(const char*,bool d=false){return d;}
    size_t getBytesLength(const char* key){return saved[space+"/"+key].size();}
    size_t getBytes(const char* key,void* out,size_t n){auto& b=saved[space+"/"+key];if(b.size()!=n)return 0;memcpy(out,b.data(),n);return n;}
    template<class T> void putUChar(const char*,T){}
    template<class T> void putFloat(const char*,T){}
    template<class T> void putUInt(const char*,T){}
    template<class T> void putBool(const char*,T){}
    size_t putBytes(const char* key,void* in,size_t n){
        if(space=="ofvRfBudget" && ++writes==failAt)return 0;
        saved[space+"/"+key]=std::vector<byte>((byte*)in,(byte*)in+n);return n;
    }
};
std::map<std::string,std::vector<byte>> Preferences::saved;
unsigned Preferences::writes=0,Preferences::failAt=0;
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_RX_TIMEOUT -6
#define RADIOLIB_ERR_TX_TIMEOUT -5
#define RADIOLIB_SX126X_IRQ_TX_DONE 1
#define RADIOLIB_SX126X_IRQ_TIMEOUT 512
#define RADIOLIB_ERR_ADDRESS_NOT_FOUND -7
#define RADIOLIB_ERR_PACKET_TOO_LONG -8
#define RADIOLIB_SX126X_MAX_PACKET_LENGTH 255
'''
# Include safety primitives when present, so baseline regressions compile against
# the old real methods too. Assertions are always against the application seam.
for name in ('NumericPayload.h', 'ControlLease.h', 'RadioTxPolicy.h'):
    if (ROOT/'src'/name).exists():
        prefix += f'\n#include "{name}"\n'
prefix += stripped('Frisquet/NetworkID.h')
driver_path = next((ROOT/'.pio/libdeps').glob('*/RadioLib/src/modules/SX126x/SX126x.cpp'))
driver_fsk = driver_path.read_text().split('case RADIOLIB_MODEM_FSK: {', 1)[1].split('}', 1)[0]
prefix += r'''
uint32_t driverTimeOnAir(size_t len){
    struct {struct {float bitRate=25.0;} fsk;} dr; // RadioLib FSKRate_t is kbps.
    struct {struct {uint8_t crcLength=2,syncWordLength=32,preambleLength=16;} fsk;} pc;
''' + driver_fsk + '}\n'
prefix += r'''
struct Config {
    NetworkID network;unsigned saves=0;
    bool useConnect(){return false;} bool useDS18B20(){return false;}
    NetworkID getNetworkID(){return network;} void setNetworkID(NetworkID n){network=n;} void save(){++saves;}
};
struct SX1262 {
    uint32_t irq=0;
    uint32_t getIrqFlags(){return irq;}
    virtual int16_t finishTransmit(){irq=0;return 0;}
};
struct Chip:SX1262 {
    unsigned transmissions=0;int txError=0;
    bool txDone=false,reportDone=true;unsigned latency=0;int toaError=0;
    std::function<void()> onTransmit;
    int standby(){return 0;} int setCRC(int,int,int,bool){return 0;}
    int variablePacketLengthMode(size_t){return 0;}
    std::vector<std::vector<byte>> sent;
    uint32_t getTimeOnAir(size_t length){return uint32_t(driverTimeOnAir(length)+toaError);}
    int16_t finishTransmit() override;
    int transmit(byte* data,size_t length){
        if(onTransmit)onTransmit();++transmissions;sent.emplace_back(data,data+length);
        tick+=latency;irq=reportDone?RADIOLIB_SX126X_IRQ_TX_DONE:RADIOLIB_SX126X_IRQ_TIMEOUT;
        finishTransmit();return txError;
    }
};
struct Radio {
    static constexpr size_t MAX_RX_PACKET_LENGTH=63;
    Chip _radio;NetworkID network;std::vector<std::vector<byte>> frames;
    std::function<int16_t(byte*,size_t)> tx;
    std::vector<byte> last;unsigned starts=0;
    float getLastPacketRSSI(){return -50;}
    size_t getPacketLength(){return last.size();}
    int startReceive(){++starts;return 0;}
    int16_t setSyncWord(byte* b,size_t n){assert(n==4);network=NetworkID(b);return 0;}
    int receive(byte* out,size_t){
        tick+=500;
        if(frames.empty())return RADIOLIB_ERR_RX_TIMEOUT;
        last=frames.front();frames.erase(frames.begin());memcpy(out,last.data(),last.size());return 0;
    }
    bool hasReceivedPacket(){return !frames.empty();}
    int readData(byte* b,size_t n){return receive(b,n);}
    int16_t transmit(byte* b,size_t n){return tx?tx(b,n):_radio.transmit(b,n);}
    bool persistTxBudget();bool loadTxBudget();
'''
if (ROOT/'src/RadioTxPolicy.h').exists():
    prefix += 'RadioTxPolicy _txPolicy; bool _txReady=true;\n'
prefix += '};\n'
prefix += 'struct TxRadio:Radio { int16_t transmit(byte*,size_t); };\n'
prefix += extract(source('Radio.cpp'), 'bool Radio::persistTxBudget(')
prefix += extract(source('Radio.cpp'), 'bool Radio::loadTxBudget(')
prefix += extract(source('Radio.cpp'), 'int16_t Radio::PacketRadio::finishTransmit(').replace('Radio::PacketRadio::','Chip::')
prefix += extract(source('Radio.cpp'), 'int16_t Radio::transmit(').replace('Radio::transmit(', 'TxRadio::transmit(')
prefix += stripped('Frisquet/Utils.h')
prefix += 'String byteArrayToHexString(uint8_t*,int){return "hex";}\n'
prefix += 'void logRadio(bool,byte*,size_t,float=0){}\n'
prefix += stripped('Buffer.h')
prefix += stripped('Frisquet/FrisquetRadio.h').replace('    private:', '    public:')
prefix += stripped('Frisquet/FrisquetDevice.h').replace('    private:', '    public:').replace('    protected:', '    public:')
prefix += stripped('Frisquet/Zone.h').replace('    private:', '    public:')
prefix += r'''
struct Chaudiere {
    enum MODE_ECS { INCONNU };
    struct ETAT_CHAUDIERE {bool arretChauffage=false;String getLibelle(){return "state";}} state;
    void setEtatChaudiere(byte b){state.arretChauffage=b&4;}
    ETAT_CHAUDIERE getEtatChaudiere(){return state;}
    void setTemperatureCDC(float){} void publishMqtt(){}
};
'''
prefix += stripped('Frisquet/RadioSchedule.h')
prefix += stripped('Frisquet/Connect.h').replace('    private:', '    public:')
prefix += 'unsigned ecsUpdates=0;void Connect::setModeECSFromRaw(uint8_t){++ecsUpdates;}\n'
prefix += extract(source('Frisquet/Connect.cpp'), 'bool Connect::recupererModeECS(')
prefix += 'struct DS18B20 {bool isReady(){return false;} bool getTemperature(float&){return false;}};\n'
prefix += stripped('Frisquet/SondeExterieure.h')
prefix += stripped('Frisquet/Satellite.h').replace('    private:', '    public:')
prefix += '\n#include "Frisquet/ZoneProgram.h"\n'
prefix += stripped('Frisquet/FrisquetRadio.cpp')
prefix += stripped('Frisquet/FrisquetDevice.cpp')
prefix += stripped('Frisquet/Zone.cpp')
prefix += stripped('Frisquet/Satellite.cpp')
prefix += stripped('Frisquet/SondeExterieure.cpp')
prefix += r'''
struct FrisquetManager {
    FrisquetRadio& r;Config& c;
    FrisquetManager(FrisquetRadio& radio,Config& config):r(radio),c(config){}
    FrisquetRadio& radio(){return r;} Config& config(){return c;}
    bool recupererNetworkID();
};
'''
prefix += extract(source('FrisquetManager.cpp'), 'bool FrisquetManager::recupererNetworkID(')
prefix += r'''
uint32_t esp_random(){static uint32_t n=1;return ++n;}
struct ConnectionHarness {
    struct Socket {
        bool online=false,allow=true;
        bool connected(){return online;}
        bool connect(const char*,const char*,const char*,const char*,int,bool,const char*,bool){online=allow;return online;}
        void subscribe(const char*){}
    } _mqtt;
    struct {String clientId="test",username,password,baseTopic="frisquet";bool cleanSession=true;} _opts;
    uint32_t _connectionGeneration=0;String _controlSession;
    std::map<String,MqttDevice*> _devices;
    std::vector<String> _removedDiscoveryTopics;
    std::map<String,std::function<void(const String&)>> _commandHandlers;
    std::map<String,String> published;
    static String compose(std::initializer_list<String> p){return MqttManager::compose(p);}
    void publishRaw(const String& t,const String& p,int,bool){published[t]=p;}
    void publishAvailability(MqttDevice&,bool){}
    void publishEntityDiscovery(MqttEntity&){}
    void reconnect();
};
'''
prefix += extract(source('MQTT/MqttManager.h'), 'void reconnect(').replace('void reconnect(', 'void ConnectionHarness::reconnect(')

checks = r'''
bool equal(NetworkID a,NetworkID b){return !memcmp(a.bytes,b.bytes,4);}
std::vector<byte> association(byte id=8){return {id,0x80,0x12,1,1,0x41,4,1,2,3,4};}
std::vector<byte> reply(byte kind=3,byte assoc=0x12){
    std::vector<byte> r(49,0);r[0]=8;r[1]=0x80;r[2]=assoc;r[3]=1;r[4]=0x81;r[5]=kind;r[6]=42;
    r[11]=0x26;r[12]=0x10;r[13]=0x28;r[14]=0x21;r[15]=0x10;r[16]=0;
    r[17]=0x28;r[19]=0;r[20]=205;r[21]=0;r[22]=210;r[24]=0x06;return r;
}
void setup(Zone& z){
    z.setSource(Zone::SATELLITE_VIRTUEL);z.setPilotageActif(true);z.begin();
    z.mqtt().command("temperatureConfort","21");z.mqtt().command("mode","Confort");
    z.mqtt().command("temperatureConsigne","21");z.mqtt().command("temperatureAmbiante","18");
}
void pairing(){
    for(byte id:{byte(8),byte(9),byte(10),byte(0x7e),byte(0x20)}){
        FrisquetRadio r;Config c;MqttManager m;FrisquetDevice d(r,c,m,id);
        NetworkID old(9,8,7,6),out(6,6,6,6);byte assoc=0x66;r.setNetworkID(old);
        r.frames.push_back(association(id));r._radio.txError=-1;
        assert(!d.associer(out,assoc)); // all confirmation TX failures must fail
        assert(equal(r.network,old)&&equal(out,NetworkID(6,6,6,6))&&assoc==0x66&&r.starts);
    }
    for(int mode=0;mode<7;++mode){
        FrisquetRadio r;Config c;MqttManager m;FrisquetDevice d(r,c,m,8);
        NetworkID old(9,8,7,6),out=old;byte assoc=0x66;r.setNetworkID(old);
        auto f=association();
        if(mode==0)f.resize(10);if(mode==1)f[0]=9;if(mode==2)f[6]=3;
        if(mode==3)f[2]=255;if(mode==4)f[4]=0x81;if(mode==5)f[1]=0x7e;
        if(mode==6)for(int i=7;i<11;++i)f[i]=0;
        r.frames.push_back(f);tick=UINT32_MAX-1000;
        assert(!d.associer(out,assoc));assert(equal(r.network,old)&&assoc==0x66);
    }
    FrisquetRadio r;Config c;MqttManager m;FrisquetDevice d(r,c,m,8);
    NetworkID out;byte assoc=255;r.frames.push_back(association());tick=UINT32_MAX-1000;
    assert(d.associer(out,assoc)&&equal(out,NetworkID(1,2,3,4))&&assoc==0x12);
    assert(r._radio.transmissions>0);
    Zone z1(8,m),z2(9,m),z3(10,m);Chaudiere boiler;Connect connect(r,c,m,z1,z2,z3,boiler);
    connect._initialReadPending=false;r.frames.push_back(association(0x7e));
    assert(connect.associer(out,assoc)&&connect._initialReadPending);
    connect.setIdAssociation(0x12);connect.setIdMessage(0);
    auto malformed=reply();malformed[0]=0x7e;malformed.resize(8);malformed[6]=1;
    r.frames.push_back(malformed);assert(!connect.recupererModeECS()&&ecsUpdates==0);
    connect.setIdMessage(0);auto valid=reply();valid[0]=0x7e;valid.resize(9);valid[6]=2;
    r.frames.push_back(valid);assert(connect.recupererModeECS()&&ecsUpdates==1);
}
void observed(){
    FrisquetRadio r;Config c;MqttManager m;Zone z(8,m);Chaudiere b;Satellite s(r,c,m,z,b);
    s.setIdAssociation(0x12);
    auto shortReply=reply();shortReply.resize(10);shortReply[6]=3;
    r.frames.push_back(shortReply);assert(!s.recupererInfosChaudiere());
    auto wrong=reply(3,0x13);r.frames.push_back(wrong);assert(!s.recupererInfosChaudiere());
    z.setTemperatureConsigne(24);z.setTemperatureAmbiante(18);z.setMode(Zone::CONFORT);
    for(int field:{0,1,2,3,4,5,6}) {
        s.setIdMessage(0);auto bad=reply();
        if(field<6)bad[field]^=1;else bad[field]=41;
        r.frames.push_back(bad);assert(!s.recupererInfosChaudiere());assert(!s._observedZ1.valid);
    }
    s.setIdMessage(0);r.frames.push_back(reply());assert(s.recupererInfosChaudiere());
    assert(s._observedZ1.valid && s._observedZ1.ambient==20.5f && s._observedZ1.setpoint==21 && s._observedZ1.rawMode==6);
    assert(z.getTemperatureConsigne()==24 && z.getTemperatureAmbiante()==18 && z.getMode()==Zone::CONFORT);
    auto ageStart=tick;tick+=1000;s.updateObservationAge();assert(s._observedZ1.ageMs==1000);
    tick=UINT32_MAX-5;s._observedZ1.tick=tick;tick=10;s.updateObservationAge();assert(s._observedZ1.ageMs==1016);
    (void)ageStart;
    s._initialReadPending=false;NetworkID network;byte assoc=255;r.frames.push_back(association());
    assert(s.associer(network,assoc));assert(s._initialReadPending && !s._observedZ1.valid);
    // A failed initial read is retried, never consumed and followed by a write.
    setup(z);s.begin(true);s.setIdAssociation(0x12);s.setIdMessage(0);r._radio.sent.clear();
    s.loop();assert(s._initialReadPending);
    for(auto& packet:r._radio.sent)if(packet[5]==0x17) {
        assert(packet.size()==23 && packet[15]==1 && packet[16]==44 && packet[17]==0 && packet[18]==50);
    }
    tick+=60000;s.setIdMessage(0);r.frames.push_back(reply());
    auto cdc=reply();cdc.resize(9);cdc[3]=2;cdc[6]=2;r.frames.push_back(cdc);
    auto init=reply(0x17);init[3]=3;r.frames.push_back(init);s.loop();
    assert(!s._initialReadPending && s._observedZ1.valid);
}
void numeric(){
    MqttManager m;Zone z(8,m);setup(z);
    for(auto field:{"temperatureAmbiante","temperatureConfort","temperatureReduit","temperatureHorsGel","temperatureBoost","temperatureConsigne"}){
        float before=field==std::string("temperatureAmbiante")?z.getTemperatureAmbiante():z.getTemperatureConsigne();
        for(auto bad:{"","junk","nan","NaN","inf","-inf","0junk","20 C","1e9999"," 21","21 ","0x15"}){
            const float profiles[]={z.getTemperatureConfort(),z.getTemperatureReduit(),z.getTemperatureHorsGel(),z.getTemperatureBoost()};
            m.command(field,bad);
            assert(z.getTemperatureAmbiante()==18);
            assert(z.getTemperatureConsigne()==21);
            assert(z.getTemperatureConfort()==profiles[0] && z.getTemperatureReduit()==profiles[1]
                   && z.getTemperatureHorsGel()==profiles[2] && z.getTemperatureBoost()==profiles[3]);
        }
        (void)before;
    }
    m.command("temperatureAmbiante","0");assert(z.getTemperatureAmbiante()==0);
    m.command("temperatureAmbiante","50");assert(z.getTemperatureAmbiante()==50);
    m.command("temperatureConsigne","30");assert(z.getTemperatureConsigne()==30);
    m.command("temperatureConsigne","30.1");assert(z.getTemperatureConsigne()==30);
    m.command("temperatureAmbiante",String("20\0junk",7));assert(z.getTemperatureAmbiante()==50);
    float value=123;assert(!NumericPayload::parse("-0.1",4,0,50,value)&&value==123);
    assert(NumericPayload::parse("+2.05e1",7,0,50,value)&&value==20.5f);
    assert(NumericPayload::parse(".5",2,0.5,5,value)&&value==0.5f);
    FrisquetRadio r;Config c;SondeExterieure probe(r,c,m);probe.begin();probe.setTemperatureExterieure(12);
    for(auto bad:{"", "junk", "nan", "inf", "-31", "81", "1 C"}) {
        m.handlers.at("frisquet/sondeExterieure/temperatureExterieure/set")(bad);
        assert(probe.getTemperatureExterieure()==12);
    }
    m.handlers.at("frisquet/sondeExterieure/temperatureExterieure/set")("-30");assert(probe.getTemperatureExterieure()==-30);
    m.handlers.at("frisquet/sondeExterieure/temperatureExterieure/set")("80");assert(probe.getTemperatureExterieure()==80);
}
void deadman(){
    FrisquetRadio r;Config c;MqttManager m;Zone z(8,m);Chaudiere b;Satellite s(r,c,m,z,b);
    setup(z);s.setIdAssociation(0x12);s._modeVirtuel=true;
    // Simulated boot restoration of stale desired state: never a lease.
    z.loadConfig();z.setMode(Zone::CONFORT);z.setTemperatureConsigne(21);z.setTemperatureAmbiante(18);
    auto response=reply(0x17);r.frames.push_back(response);
    bool result=s.envoyerConsigne();(void)result;
    // Actual sendInit payload captured below in expanded post-fix assertions.
    assert(r._radio.transmissions==0); // initial read qualification must precede writes
    s._initialReadPending=false;Satellite::Demand demand;
    assert(s.prepareDemand(demand));assert(demand.temperatureConsigne.toFloat()==5 && demand.temperatureAmbiante.toFloat()==30);
    // Ambient/target alone cannot renew the stale persisted heating mode.
    m.command("temperatureAmbiante","18");m.command("temperatureConsigne","21");
    m.command("controlHeartbeat","1:1");assert(!z.controlCurrent());
    m.command("mode","garbage");m.command("controlHeartbeat","1:1");assert(!z.controlCurrent());
    m.command("mode","Confort");m.command("controlHeartbeat","1:1");assert(z.controlCurrent());
    const uint32_t renewed=tick;
    assert(s.prepareDemand(demand) && demand.temperatureConsigne.toFloat()==21 && demand.temperatureAmbiante.toFloat()==18);
    tick=renewed+ControlLease::LEASE_MS-1;clockNow-=86400;assert(z.controlCurrent());clockNow+=172800;assert(z.controlCurrent());
    m.command("temperatureAmbiante","18");m.command("temperatureConsigne","21");
    m.command("controlHeartbeat","1:2"); // no new mode since last heartbeat
    tick=renewed+ControlLease::LEASE_MS;assert(!z.controlCurrent());
    assert(s.prepareDemand(demand)&&demand.temperatureConsigne.toFloat()==5&&demand.temperatureAmbiante.toFloat()==30);
    assert(z.getTemperatureConsigne()==21&&z.getMode()==Zone::CONFORT);
    m.command("mode","Confort");m.command("controlHeartbeat","1:1");assert(!z.controlCurrent()); // replay
    m.command("controlHeartbeat","1:2");assert(z.controlCurrent());
    m.online=false;assert(!z.controlCurrent());m.online=true;++m.generation;
    m.command("temperatureAmbiante","18");m.command("temperatureConsigne","21");m.command("mode","Confort");
    m.command("controlHeartbeat","1:3");assert(!z.controlCurrent()); // old session, even higher sequence
    m.command("controlHeartbeat","2:1");assert(z.controlCurrent());
    z.loadConfig();assert(!z.controlCurrent()); // NVS never re-arms
    m.command("temperatureConfort","21");
    tick=UINT32_MAX-500;
    m.command("temperatureAmbiante","18");m.command("mode","Confort");m.command("temperatureConsigne","21");
    m.command("controlHeartbeat","2:2");assert(z.controlCurrent());const auto wrapped=tick;
    tick=wrapped+ControlLease::LEASE_MS-1;assert(z.controlCurrent());++tick;assert(!z.controlCurrent());
    // Fresh commands older than one heartbeat interval cannot qualify a renewal.
    m.command("temperatureAmbiante","18");m.command("mode","Confort");m.command("temperatureConsigne","21");
    tick+=ControlLease::HEARTBEAT_MS+1;m.command("controlHeartbeat","2:3");assert(!z.controlCurrent());
    // No scalar payload or malformed heartbeat can make a lease out of an expired one.
    for(auto bad:{"", "2:", "2:nan", "2:18446744073709551616", "3:4", "2:4junk"})m.command("controlHeartbeat",bad);
    assert(!z.controlCurrent());
    z.setMode(Zone::INCONNU);z.setTemperatureAmbiante(NAN);z.setTemperatureConsigne(NAN);
    assert(s.prepareDemand(demand)&&demand.temperatureConsigne.toFloat()==5&&demand.temperatureAmbiante.toFloat()==30);
    // The actual send path sends the safe payload, not merely 'stop TX'.
    r.frames.clear();s.setIdMessage(0);r.frames.push_back(reply(0x17));
    assert(s.envoyerConsigne());const auto& sent=r._radio.sent.back();assert(sent.size()==23);
    assert(sent[15]==1 && sent[16]==44 && sent[17]==0 && sent[18]==50); // 30 C / 5 C
}
void rf(){
    TxRadio r;byte data[11]={};
    r._txReady=false;
    assert(r.transmit(data,sizeof(data))!=0);
    assert(r._radio.transmissions==0); // unqualified HP/antenna mapping cannot transmit
    r._txReady=true;r._txPolicy.reset(0);tick=100;r._radio.txError=-99;
    const unsigned permitted=(RadioTxPolicy::BUDGET_US-RadioTxPolicy::SAFETY_RESERVE_US)/RadioTxPolicy::chargeUs(sizeof(data));
    for(unsigned n=0;n<permitted;++n)assert(r.transmit(data,sizeof(data))==-99);
    assert(r.transmit(data,sizeof(data))==RadioTxPolicy::AIRTIME_LIMIT && r._radio.transmissions==permitted);
    TxRadio reboot;reboot._txReady=true;assert(reboot.loadTxBudget());
    assert(reboot.transmit(data,sizeof(data))==RadioTxPolicy::AIRTIME_LIMIT);
    tick+=RadioTxPolicy::HOUR_MS+60000U-1;assert(r.transmit(data,sizeof(data))==RadioTxPolicy::AIRTIME_LIMIT);
    tick=62*60000U;assert(r.transmit(data,sizeof(data))==-99); // retain the whole packet at minute edges
    RadioTxPolicy wrap;wrap.reset(UINT32_MAX-1000);assert(wrap.reserve(UINT32_MAX-1,11));
    assert(wrap.reserve(uint32_t(UINT32_MAX-1000+RadioTxPolicy::HOUR_MS),11));
    // Pair confirmations and read retries reach the SAME actual TX seam.
    TxRadio shared;shared._txReady=true;shared._txPolicy.reset(0);tick=RadioTxPolicy::HOUR_MS;
    FrisquetRadio protocol;Config c;MqttManager m;FrisquetDevice d(protocol,c,m,8);
    protocol.tx=[&](byte* b,size_t n){return shared.transmit(b,n);};
    protocol.frames.push_back(association());NetworkID network;byte assoc=255;
    assert(d.associer(network,assoc));assert(shared._radio.transmissions==10);
    byte buffer[255];size_t length=sizeof(buffer);const auto before=shared._radio.transmissions;
    assert(protocol.sendAsk(8,0x80,0x12,1,1,0xA029,0x15,buffer,length,0)!=0);
    assert(shared._radio.transmissions==before);
    length=sizeof(buffer);assert(protocol.sendAsk(8,0x80,0x12,1,1,0xA029,0x15,buffer,length,5)!=0);
    assert(shared._radio.transmissions==before+5);
    shared._txReady=false;length=sizeof(buffer);
    assert(protocol.sendAsk(8,0x80,0x12,1,1,0xA029,0x15,buffer,length,5)==RadioTxPolicy::INHIBITED);
    assert(shared._radio.transmissions==before+5);
}
std::vector<byte> noHeat(){
    return {0x80,8,0x12,1,1,0x17,0xA0,0x29,0,0x15,0xA0,0x2F,0,4,8,1,44,0,50,0,0x10,0,0};
}
void bootBudget(){
    TxRadio r;r._txReady=true;tick=100;r._txPolicy.reset(0);
    auto safe=noHeat();
    assert(r.transmit(safe.data(),safe.size())==0); // no unconditional boot RF quarantine
}
void safetyReserve(){
    TxRadio r;r._txReady=true;r._txPolicy.reset(0);tick=RadioTxPolicy::HOUR_MS;
    byte ordinary[23]={};r._radio.txError=-99;
    while(r.transmit(ordinary,sizeof(ordinary))==-99){}
    auto safe=noHeat();const auto spent=r._txPolicy.usedUs();
    TxRadio reboot;reboot._txReady=true;assert(reboot.loadTxBudget());reboot._radio.txError=-99;
    const unsigned allowance=(RadioTxPolicy::BUDGET_US-spent)/RadioTxPolicy::chargeUs(safe.size());
    assert(allowance>=8);
    for(unsigned n=0;n<allowance;++n)assert(reboot.transmit(safe.data(),safe.size())==-99);
    assert(reboot.transmit(safe.data(),safe.size())==RadioTxPolicy::AIRTIME_LIMIT);
    assert(reboot._radio.transmissions==allowance && reboot._txPolicy.usedUs()<=RadioTxPolicy::BUDGET_US);
    printf("reserve: after ordinary exhaustion and reboot, %u ambiguous no-heat attempts admitted\n",allowance);
}
void cadence(){
    TxRadio accounted;accounted._txReady=true;accounted._txPolicy.reset(0);
    tick=RadioTxPolicy::HOUR_MS;
    FrisquetRadio r;Config c;MqttManager m;Zone z(8,m);Chaudiere b;Satellite s(r,c,m,z,b);
    setup(z);s.begin(true);s.setIdAssociation(0x12);
    r.tx=[&](byte* data,size_t size){
        int16_t result=accounted.transmit(data,size);
        if(result==0){
            auto response=reply(data[5]);response[3]=data[3];
            if(data[5]==3 && data[6]==0xA0 && data[7]==0x5E){response.resize(9);response[6]=2;}
            r.frames.push_back(response);
        }
        if(result!=0)printf("cadence TX refused: code=%d length=%zu at=%u\n",result,size,tick);
        fflush(stdout);return result;
    };
    const auto start=tick;
    for(unsigned minute=0;minute<60;++minute){
        tick=start+minute*60000U;clockNow+=60;
        m.command("mode","Confort");m.command("temperatureConsigne","21");m.command("temperatureAmbiante","18");
        m.command("controlHeartbeat",String("1:")+String(minute+1));
        const unsigned before=accounted._radio.transmissions;s.loop();
        assert(!s._initialReadPending && accounted._radio.transmissions>before);
        if(minute%5==4)assert(s.recupererTemperatureCDC()); // native 2/10-minute CDC cadence
    }
    m.online=false;tick=start+3600000U;s.loop();
    unsigned normal=0,safety=0,reads=0;
    for(const auto& frame:accounted._radio.sent){
        if(frame[5]==3)++reads;
        if(frame.size()!=23)continue;
        if(RadioTxPolicy::isNoHeat(frame.data(),frame.size()))++safety;
        else {assert(frame[15]==0 && frame[16]==180 && frame[17]==0 && frame[18]==210);++normal;}
    }
    assert(normal==60 && safety==1 && reads>=14);
    printf("cadence: normal=%u safe=%u reads=%u total=%zu (60 heartbeats, native + 12 extra CDC reads)\n",normal,safety,reads,accounted._radio.sent.size());
    printf("cadence accounted_us=%u ceiling_us=%u\n",accounted._txPolicy.usedUs(),RadioTxPolicy::BUDGET_US);
}
void budgetRecovery(){
    Preferences::saved.clear();Preferences::writes=0;Preferences::failAt=0;
    TxRadio legacy;legacy._txReady=true;tick=0;assert(legacy.loadTxBudget());
    auto safe=noHeat();
    assert(legacy.transmit(safe.data(),safe.size())==RadioTxPolicy::AIRTIME_LIMIT); // truly unknown legacy spend
    tick=62*60000U;assert(legacy.transmit(safe.data(),safe.size())==0);
    const auto confirmed=legacy._txPolicy.usedUs();assert(confirmed<RadioTxPolicy::chargeUs(safe.size()));
    TxRadio reboot;reboot._txReady=true;tick=1;assert(reboot.loadTxBudget());
    assert(reboot._txPolicy.usedUs()==confirmed && reboot.transmit(safe.data(),safe.size())==0);
    const auto before=reboot._txPolicy.usedUs();
    reboot._radio.onTransmit=[&](){
        TxRadio interrupted;assert(interrupted.loadTxBudget());
        assert(interrupted._txPolicy.usedUs()==before+RadioTxPolicy::chargeUs(safe.size()));
    };
    reboot._radio.txError=-99;assert(reboot.transmit(safe.data(),safe.size())==-99);
    reboot._radio.onTransmit=nullptr;
    TxRadio afterFailure;afterFailure._txReady=true;assert(afterFailure.loadTxBudget());
    assert(afterFailure._txPolicy.usedUs()==before+RadioTxPolicy::chargeUs(safe.size()));
    Preferences::failAt=Preferences::writes+1;
    const unsigned sent=afterFailure._radio.transmissions;
    assert(afterFailure.transmit(safe.data(),safe.size())==RadioTxPolicy::INHIBITED && afterFailure._radio.transmissions==sent);
    Preferences::failAt=0;
    // No success refund without a TX_DONE receipt, even when the driver returns 0.
    TxRadio ambiguous;ambiguous._txReady=true;ambiguous._txPolicy.reset(0);ambiguous._radio.reportDone=false;
    assert(ambiguous.transmit(safe.data(),safe.size())==RADIOLIB_ERR_TX_TIMEOUT);
    assert(ambiguous._txPolicy.usedUs()==RadioTxPolicy::chargeUs(safe.size()));
    // Failed post-success persistence retains the larger write-ahead amount on flash.
    TxRadio reduction;reduction._txReady=true;reduction._txPolicy.reset(0);
    Preferences::failAt=Preferences::writes+2;
    assert(reduction.transmit(safe.data(),safe.size())==RadioTxPolicy::INHIBITED);
    Preferences::failAt=0;TxRadio recovered;assert(recovered.loadTxBudget());
    assert(recovered._txPolicy.usedUs()==RadioTxPolicy::chargeUs(safe.size()));
    // A bad format/timing estimate or malformed safety payload cannot obtain reserve.
    TxRadio wrong;wrong._txReady=true;wrong._radio.toaError=1000;
    assert(wrong.transmit(safe.data(),safe.size())==RadioTxPolicy::INHIBITED && wrong._radio.transmissions==0);
    assert(RadioTxPolicy::isNoHeat(safe.data(),safe.size()));
    for(unsigned field=0;field<safe.size();++field){
        if(field==2 || field==3)continue;
        auto bad=safe;bad[field]^=1;assert(!RadioTxPolicy::isNoHeat(bad.data(),bad.size()));
    }
    auto bad=safe;bad[2]=255;assert(!RadioTxPolicy::isNoHeat(bad.data(),bad.size()));
    assert(!RadioTxPolicy::isNoHeat(safe.data(),safe.size()-1));
    // Recovered/corrupt spend never gains credit from wall time or repeated resets.
    Preferences::saved["ofvRfBudget/used"]={1,2,3};
    TxRadio corrupt;corrupt._txReady=true;assert(corrupt.loadTxBudget());
    assert(corrupt._txPolicy.usedUs()==RadioTxPolicy::BUDGET_US);
    assert(corrupt.transmit(safe.data(),safe.size())==RadioTxPolicy::AIRTIME_LIMIT);
    TxRadio resetAgain;assert(resetAgain.loadTxBudget());assert(resetAgain._txPolicy.usedUs()==RadioTxPolicy::BUDGET_US);
    TxRadio wrapped;wrapped._txReady=true;tick=UINT32_MAX-5;wrapped._txPolicy.reset(tick);
    wrapped._radio.latency=10;assert(wrapped.transmit(safe.data(),safe.size())==0);
    assert(wrapped._txPolicy.usedUs()==12343);
    TxRadio late;late._txReady=true;late._radio.latency=58;
    assert(late.transmit(safe.data(),safe.size())==0 && !late._txReady);
    assert(late._txPolicy.usedUs()==RadioTxPolicy::chargeUs(safe.size()));
    assert(late.transmit(safe.data(),safe.size())==RadioTxPolicy::INHIBITED);
    printf("budget: write-ahead, reset, NVS failure, no ambiguous refund, reserve classifier and units verified\n");
}
void recovery(){
    FrisquetRadio r;Config c;FrisquetManager manager(r,c);NetworkID old(9,8,7,6);r.setNetworkID(old);c.network=old;
    tick=UINT32_MAX-1000;r.frames.push_back(association(0x45));
    assert(!manager.recupererNetworkID());assert(equal(r.network,old)&&equal(c.network,old)&&c.saves==0);
    r.frames.push_back(association());assert(manager.recupererNetworkID());
    assert(equal(r.network,NetworkID(1,2,3,4))&&equal(c.network,r.network)&&c.saves==1&&r.starts);
    ConnectionHarness mqtt;mqtt._mqtt.allow=false;mqtt.reconnect();
    assert(mqtt._connectionGeneration==0&&mqtt._controlSession.empty());
    mqtt._mqtt.allow=true;mqtt.reconnect();const String oldSession=mqtt._controlSession;
    assert(oldSession.size()==16&&mqtt._connectionGeneration==1&&mqtt.published.at("frisquet/controlSession")==oldSession);
    mqtt.reconnect();assert(mqtt._connectionGeneration==1);
    mqtt._mqtt.online=false;mqtt.reconnect();assert(mqtt._connectionGeneration==2&&mqtt._controlSession!=oldSession);
}
int main(int argc,char** argv){
    assert(argc==2);std::string c=argv[1];
    if(c=="pairing")pairing();else if(c=="observed")observed();else if(c=="numeric")numeric();
    else if(c=="deadman")deadman();else if(c=="rf")rf();else if(c=="recovery")recovery();
    else if(c=="bootBudget")bootBudget();else if(c=="safetyReserve")safetyReserve();else if(c=="cadence")cadence();
    else if(c=="budgetRecovery")budgetRecovery();else assert(false);
    printf("PASS %s (real application code, simulated services/radio)\n",argv[1]);
}
'''
with tempfile.TemporaryDirectory(prefix='commissioning-') as directory:
    cpp = Path(directory)/'test.cpp'
    cpp.write_text(prefix+checks)
    binary = Path(directory)/'test'
    subprocess.run(['c++','-std=c++11','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-I',str(ROOT/'src'),str(cpp),'-o',str(binary)],check=True)
    failures = []
    groups = sys.argv[1:] or ['pairing','observed','numeric','deadman','rf','recovery','bootBudget','safetyReserve','cadence','budgetRecovery']
    for group in groups:
        result = subprocess.run([str(binary),group])
        if result.returncode:
            failures.append(group)
    print(f'{len(groups)-len(failures)}/{len(groups)} groups passed; failed={failures}',flush=True)
    raise SystemExit(bool(failures))
