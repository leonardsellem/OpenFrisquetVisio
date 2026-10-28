#pragma once

#include "FrisquetDevice.h"
#include <Preferences.h>
#include "Logs.h"
#include "Zone.h"
#include "Chaudiere.h"
#include "RadioSchedule.h"

class Satellite : public FrisquetDevice {
    
    public:
        static constexpr uint8_t MODE_CONNECT_COMPAT_XOR = 0x20;

        enum MODE : uint8_t {
            REDUIT_PERMANENT = 0x00,
            CONFORT_PERMANENT = 0x01,
            REDUIT_DEROGATION = 0x02,
            CONFORT_DEROGATION = 0x03,
            REDUIT_AUTO = 0x04,
            CONFORT_AUTO = 0x05,
            HORS_GEL = 0x10,
            INCONNU = 0xFF,
        };

        Satellite(FrisquetRadio& radio, Config& cfg, MqttManager& mqtt, Zone& zone, Chaudiere& chaudiere)
            : FrisquetDevice(radio, cfg, mqtt, zone.getIdZone()), _zone(zone), _chaudiere(chaudiere) {}
        void loadConfig();
        void saveConfig();

        void loop();
        void begin(bool modeVirtuel = false);
        void publishMqtt();

        bool envoyerConsigne();
        bool envoyerTemperatureAmbiante();
        bool recupererInfosChaudiere();
        bool recupererTemperatureCDC();

        bool onReceive(byte* donnees, size_t length);

        MODE getMode();
        void setMode(MODE mode);
        void setModeFromRaw(uint8_t rawMode);
        String getNomMode();

        void setEcrasement(bool ecrasement) {
            _ecrasement = ecrasement;
            if(_zone.getSource() == Zone::SOURCE::SATELLITE_PHYSIQUE) _zone.setPilotageActif(ecrasement);
        }
        bool getEcrasement() { return _ecrasement; }

        void setEtatChaudiere(byte etatChaudiere) { _chaudiere.setEtatChaudiere(etatChaudiere); }
        Chaudiere::ETAT_CHAUDIERE getEtatChaudiere() { return _chaudiere.getEtatChaudiere(); }

        uint8_t getNumeroZone() {
            return _zone.getNumeroZone();
        }

    private:
        void onAssociationAttempt() override;
        void updateObservationAge();
        struct Demand {
            temperature16 temperatureAmbiante;
            temperature16 temperatureConsigne;
            uint8_t i1 = 0;
            uint8_t mode = 0;
            fword options = (uint16_t)0;
        };
        bool prepareDemand(Demand& payload);
        struct {
            bool valid = false;
            bool beforeWrite = false;
            float ambient = NAN;
            float setpoint = NAN;
            uint8_t rawMode = 0xFF;
            uint64_t ageMs = 0;
            uint32_t tick = 0;
        } _observedZ1;
        uint32_t _lastReadAttempt = 0;
        bool _readAttempted = false;
        bool _writeAttempted = false;
        uint32_t _lastConsigneAttempt = 0;
        bool _consigneAttempted = false;
        bool _lastDemandSafe = true;
        uint32_t _lastSafetyPublish = 0;
        Zone& _zone;
        Chaudiere& _chaudiere;

        uint32_t _lastEnvoiConsigne = 0;
        uint32_t _lastRecuperationTemperatureCDC = 0;
        RadioSchedule _cdcSchedules[2];
        bool _initialReadPending = true;
        bool _modeVirtuel = false;
        bool _ecrasement = false;
        bool _modeCompatConnect = false;

        MODE decodeModeFromRaw(uint8_t rawMode, bool* compatConnect = nullptr) const;
        uint8_t encodeModeForSend(MODE mode) const;
        struct {
            MqttEntity ecrasementConsigne;
            MqttEntity boilerObserved;
            MqttEntity controlSafety;
        } _mqttEntities;
};
