#include "Satellite.h"
#include <math.h>
#include "../Buffer.h"

void Satellite::loadConfig() {
    getPreferences().begin((String("satCfgZ") + String(getNumeroZone())).c_str(), false);
    setIdAssociation(getPreferences().getUChar("idAssociation", 0xFF));
    getPreferences().end();
}

void Satellite::saveConfig() {   
    getPreferences().begin((String("satCfgZ") + String(getNumeroZone())).c_str(), false);
    getPreferences().putUChar("idAssociation", getIdAssociation());
    getPreferences().end();
}


void Satellite::begin(bool modeVirtuel) {
    _modeVirtuel = modeVirtuel;
    onAssociationAttempt();
    loadConfig();

    info("[SATELLITE Z%d] Initialisation du satellite [MODE %s].", _zone.getNumeroZone(), _modeVirtuel ? "VIRTUEL" : "PHYSIQUE");

    // Device commun
    MqttDevice* device = mqtt().getDevice("openFrisquetVisio");

    if (_modeVirtuel && getNumeroZone() == 1) {
        auto& observed = _mqttEntities.boilerObserved;
        observed.id = "boilerObservedZ1";
        observed.name = "Observation chaudière Z1 (A029)";
        observed.component = "sensor";
        observed.stateTopic = MqttTopic(MqttManager::compose({device->baseTopic, "z1", "boilerObserved"}), 0, false);
        observed.set("value_template", "{{ value_json.valid }}");
        observed.set("json_attributes_topic", observed.stateTopic.full);
        mqtt().registerEntity(*device, observed, true);
        auto& safety = _mqttEntities.controlSafety;
        safety.id = "controlSafetyZ1";
        safety.name = "Sécurité commande Z1";
        safety.component = "sensor";
        safety.stateTopic = MqttTopic(MqttManager::compose({device->baseTopic, "z1", "controlSafety"}), 0, false);
        mqtt().registerEntity(*device, safety, true);
    }

    // SWITCH: Activation Écrasement consigne
    _mqttEntities.ecrasementConsigne.id = "ecrasementConsigneZ" + String(getNumeroZone());
    _mqttEntities.ecrasementConsigne.name = "Écrasement consigne Z" + String(getNumeroZone());
    _mqttEntities.ecrasementConsigne.component = "switch";
    _mqttEntities.ecrasementConsigne.stateTopic = MqttTopic(MqttManager::compose({device->baseTopic,"z" + String(getNumeroZone()), "ecrasementConsigne"}), 0, true);
    _mqttEntities.ecrasementConsigne.commandTopic = MqttTopic(MqttManager::compose({device->baseTopic,"z" + String(getNumeroZone()),"ecrasementConsigne", "set"}), 0, true);
    _mqttEntities.ecrasementConsigne.set("icon", "mdi:tune-variant");
    mqtt().registerEntity(*device, _mqttEntities.ecrasementConsigne, true);
    mqtt().onCommand(_mqttEntities.ecrasementConsigne, [&](const String& payload){
        if(payload.equalsIgnoreCase("ON")) { 
            info("[SATELLITE %d] Activation de l'écrasement", getNumeroZone());
            setEcrasement(true);
        } else {
            info("[SATELLITE %d] Désactivation de l'écrasement", getNumeroZone());
            setEcrasement(false);
        }
        mqtt().publishState(_mqttEntities.ecrasementConsigne, payload);
    });

}

void Satellite::loop() {
    updateObservationAge();
    bool& firstLoop = _initialReadPending;
    const bool cdcGereeParConnect = getConfig().useConnect();
    if(firstLoop && !_modeVirtuel) {
        firstLoop = false;
        publishMqtt();
        _zone.publishMqtt();
    }

    uint32_t now = millis();
    if (_modeVirtuel && uint32_t(now - _lastSafetyPublish) >= 60000U) {
        publishMqtt();
        _lastSafetyPublish = now;
    }
    if(! estAssocie() || !_modeVirtuel) {
        return;
    }

    if(firstLoop) {
        if (_readAttempted && uint32_t(now - _lastReadAttempt) < 60000U) return;
        _readAttempted = true;
        _lastReadAttempt = now;
        if (!recupererInfosChaudiere()) {
            // A read failure must not preserve a previous boot's heat forever.
            // Only the fixed no-heat direction is allowed until qualification.
            envoyerConsigne();
            return;
        }
        firstLoop = false; // No heating write before a complete matching READ.
        if (this->getId() == ID_ZONE_1 && !cdcGereeParConnect) {
            if (recupererTemperatureCDC()) {
                _lastRecuperationTemperatureCDC = millis();
            } else {
                error("[SATELLITE Z%d] Échec de la récupération initiale de la température CDC.", getNumeroZone());
            }
            _cdcSchedules[0].due(600, 180);
            _cdcSchedules[1].due(600, 480);
        }
        publishMqtt();
        if (this->getId() == ID_ZONE_1 && !cdcGereeParConnect) _chaudiere.publishMqtt();
        _zone.publishMqtt();
    }

    // Sans Connect : :03 et :08 de chaque tranche, hors créneaux des satellites.
    if (this->getId() == ID_ZONE_1 && !cdcGereeParConnect
        && (_cdcSchedules[0].due(600, 180) || _cdcSchedules[1].due(600, 480))) {
        info("[SATELLITE Z%d] Récupération de la température CDC...", getNumeroZone());
        if (recupererTemperatureCDC()) {
            _lastRecuperationTemperatureCDC = now;
            _chaudiere.publishMqtt();
        } else {
            _lastRecuperationTemperatureCDC = now <= 60000 ? 1 : now - 60000;
            error("[SATELLITE Z%d] Échec de la récupération de la température CDC.", getNumeroZone());
        }
        delay(100);
    }

    now = millis();
    const bool safe = !_zone.controlCurrent() || getEtatChaudiere().arretChauffage;
    const bool due = !_consigneAttempted || safe != _lastDemandSafe
        || _zone.getLastChange() != _zone.getLastEnvoi()
        || uint32_t(now - _lastEnvoiConsigne) >= 600000U;
    if (due && (!_consigneAttempted || uint32_t(now - _lastConsigneAttempt) >= 15000U)) {
        info("[SATELLITE Z%d] Envoi de la consigne.", getNumeroZone());
        _consigneAttempted = true;
        _lastConsigneAttempt = now;
        if(envoyerConsigne()) {
            _zone.refreshLastEnvoi();
            _lastDemandSafe = safe;
            incrementIdMessage(3);
            _lastEnvoiConsigne = now;
            _zone.publishMqtt();
            publishMqtt();
        } else {
            error("[SATELLITE Z%d] Echec de l'envoi.", getNumeroZone());
            _lastEnvoiConsigne = now <= 60000 ? 1 : now - 60000;
        }
    }
}

void Satellite::publishMqtt() {
    if (_modeVirtuel && getNumeroZone() == 1) {
        updateObservationAge();
        char state[192];
        if (_observedZ1.valid) {
            snprintf(state, sizeof(state), "{\"valid\":true,\"ambient\":%.1f,\"setpoint\":%.1f,\"rawMode\":%u,\"ageMs\":%llu,\"beforeWriteAttempt\":%s}",
                     _observedZ1.ambient, _observedZ1.setpoint, _observedZ1.rawMode,
                     (unsigned long long)_observedZ1.ageMs, _observedZ1.beforeWrite ? "true" : "false");
        } else {
            snprintf(state, sizeof(state), "{\"valid\":false}");
        }
        mqtt().publishState(_mqttEntities.boilerObserved, state);
        mqtt().publishState(_mqttEntities.controlSafety, !_initialReadPending && _zone.controlCurrent() && !getEtatChaudiere().arretChauffage
                            ? "current-control; RF inhibited" : "no-heat direction; RF inhibited");
    }
    mqtt().publishState(*mqtt().getDevice("openFrisquetVisio")->getEntity("ecrasementConsigneZ" + String(getNumeroZone())), getEcrasement() ? "ON" : "OFF");
    if(this->getId() == ID_ZONE_1) { // Seulement sur Z1 (leader)
        _chaudiere.publishMqtt();
    }
}

bool Satellite::recupererInfosChaudiere() {
    if(! estAssocie() || getNumeroZone() == 0) {
        return false;
    }

    struct donneesZones_t {
        FrisquetRadio::RadioTrameHeader header;
        uint8_t longueur;
        temperature16 temperatureExterieure;
        byte i1[2];
        uint8_t date[6];  // format reçu "YY MM DD hh mm ss"
        byte etatChaudiere; // mode chaudière à valider 0X20 Hors gel 0x24 = Hors gel contact sec 0X28 = Fonctionnement
        uint8_t jourSemaine; // format wday
        temperature16 temperatureAmbianteZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i2 = 0x00;
        uint8_t modeZ1 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i3[4] = {0x00, 0xC6, 0x00, 0xC6};
        temperature16 temperatureAmbianteZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i4 = 0x00;
        uint8_t modeZ2 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i5[4] = {0x00, 0x00, 0x00, 0x00};
        temperature16 temperatureAmbianteZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i6 = 0x00;
        uint8_t modeZ3 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i7[4] = {0x00, 0x00, 0x00, 0x00};
    } donneesZones;

    size_t length = 0;
    int16_t err;

    info("[Satellite %d] Récupération des informations chaudière.", _zone.getNumeroZone());
    
    uint8_t retry = 0;
    do {
        length = sizeof(donneesZones_t);
        err = this->radio().sendAsk(
            this->getId(), 
            ID_CHAUDIERE, 
            this->getIdAssociation(),
            this->incrementIdMessage(),
            0x01,
            0xA029,
            0x0015,
            (byte*)&donneesZones,
            length
        );
        
        if(err != RADIOLIB_ERR_NONE || length != sizeof(donneesZones_t)
            || donneesZones.longueur != sizeof(donneesZones_t) - sizeof(FrisquetRadio::RadioTrameHeader) - 1) {
            delay(30);
            continue;
        }

        const float ambient = donneesZones.temperatureAmbianteZ1.toFloat();
        const float setpoint = donneesZones.temperatureConsigneZ1.toFloat();
        if (!isfinite(ambient) || ambient < 0 || ambient > 50
            || !isfinite(setpoint) || setpoint < 5 || setpoint > 30) continue;
        // Pure read-side truth, NOT a three-preset/program download. No Zone setter.
        _observedZ1.valid = true;
        _observedZ1.beforeWrite = !_writeAttempted;
        _observedZ1.ambient = ambient;
        _observedZ1.setpoint = setpoint;
        _observedZ1.rawMode = donneesZones.modeZ1;
        _observedZ1.ageMs = 0;
        _observedZ1.tick = millis();
        setEtatChaudiere(donneesZones.etatChaudiere);
        Date date = donneesZones.date;
        setDate(date);
        
        return true;
    } while(retry++ < 1);

    return false;
}

bool Satellite::recupererTemperatureCDC() {
    if(! estAssocie() || getNumeroZone() == 0) {
        return false;
    }

    struct donneesTemperatureCDC_t {
        FrisquetRadio::RadioTrameHeader header;
        uint8_t longueurDonnees;
        temperature16 temperatureCDC;
    } donneesTemperatureCDC;

    size_t length;
    int16_t err;

    uint8_t retry = 0;
    do {
        length = sizeof(donneesTemperatureCDC_t);
        err = this->radio().sendAsk(
            this->getId(),
            ID_CHAUDIERE,
            this->getIdAssociation(),
            this->incrementIdMessage(),
            0x01,
            0xA05E,
            0x0001,
            (byte*)&donneesTemperatureCDC,
            length
        );

        if(err != RADIOLIB_ERR_NONE || length != sizeof(donneesTemperatureCDC_t)
            || donneesTemperatureCDC.longueurDonnees != sizeof(temperature16)) {
            delay(30);
            continue;
        }

        float temperatureCDC = donneesTemperatureCDC.temperatureCDC.toFloat();
        _chaudiere.setTemperatureCDC(temperatureCDC);
        debug("[SATELLITE Z%d] Température CDC : %.1f°C", getNumeroZone(), temperatureCDC);
        return true;
    } while(retry++ < 1);

    return false;
}

bool Satellite::envoyerTemperatureAmbiante() {
    // Route every virtual write through the same dead-man path.
    if (_modeVirtuel) return envoyerConsigne();
    if(! estAssocie() || getNumeroZone() == 0) {
        return false;
    }

    if(isnan(_zone.getTemperatureAmbiante()) || _zone.getNumeroZone() == 0) {
        error("[SATELLITE Z%d] Impossible d'envoyer la température ambiante, configuration incomplète.", getNumeroZone());
        return false;
    }

    struct donneesSatellite_t {
        temperature16 temperatureAmbiante;
    } payload;

    struct donneesZones_t {
        FrisquetRadio::RadioTrameHeader header;
        uint8_t longueur;
        temperature16 temperatureExterieure;
        byte i1[2];
        uint8_t date[6];  // format reçu "YY MM DD hh mm ss"
        byte etatChaudiere; // mode chaudière à valider 0X20 Hors gel 0x24 = Hors gel contact sec 0X28 = Fonctionnement
        uint8_t jourSemaine; // format wday
        temperature16 temperatureAmbianteZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i2 = 0x00;
        uint8_t modeZ1 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i3[4] = {0x00, 0xC6, 0x00, 0xC6};
        temperature16 temperatureAmbianteZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i4 = 0x00;
        uint8_t modeZ2 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i5[4] = {0x00, 0x00, 0x00, 0x00};
        temperature16 temperatureAmbianteZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i6 = 0x00;
        uint8_t modeZ3 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i7[4] = {0x00, 0x00, 0x00, 0x00};
    } donneesZones;

    
    payload.temperatureAmbiante = _zone.getTemperatureAmbiante();

    size_t length = 0;
    int16_t err;

    info("[Satellite %d] Envoi de la température ambiante %0.2f", _zone.getNumeroZone(), payload.temperatureAmbiante.toFloat());
    
    uint8_t retry = 0;
    do {
        length = sizeof(donneesZones_t);
        err = this->radio().sendInit(
            this->getId(), 
            ID_CHAUDIERE, 
            this->getIdAssociation(),
            this->incrementIdMessage(),
            0x01, 
            0xA029,
            0x0015,
            0xA02F + (0x0005 * (getNumeroZone() - 1)),
            0x0001,
            (byte*)&payload,
            sizeof(payload),
            (byte*)&donneesZones,
            length
        );
        
        if(err != RADIOLIB_ERR_NONE || length != sizeof(donneesZones_t)
            || donneesZones.longueur != sizeof(donneesZones_t) - sizeof(FrisquetRadio::RadioTrameHeader) - 1) {
            delay(30);
            continue;
        }

        setEtatChaudiere(donneesZones.etatChaudiere);
        Date date = donneesZones.date;
        setDate(date);
        
        return true;
    } while(retry++ < 1);

    return false;
}

bool Satellite::envoyerConsigne() {
    if(! estAssocie() || getNumeroZone() == 0) {
        return false;
    }

    if (_modeVirtuel && _initialReadPending && !_readAttempted) return false;
    Demand payload;

    struct donneesZones_t {
        FrisquetRadio::RadioTrameHeader header;
        uint8_t longueur;
        temperature16 temperatureExterieure;
        byte i1[2];
        uint8_t date[6];  // format reçu "YY MM DD hh mm ss"
        byte etatChaudiere; // mode chaudière à valider 0X20 Hors gel 0x24 = Hors gel contact sec 0X28 = Fonctionnement
        uint8_t jourSemaine; // format wday
        temperature16 temperatureAmbianteZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i2 = 0x00;
        uint8_t modeZ1 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i3[4] = {0x00, 0xC6, 0x00, 0xC6};
        temperature16 temperatureAmbianteZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i4 = 0x00;
        uint8_t modeZ2 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i5[4] = {0x00, 0x00, 0x00, 0x00};
        temperature16 temperatureAmbianteZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        temperature16 temperatureConsigneZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
        byte i6 = 0x00;
        uint8_t modeZ3 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
        byte i7[4] = {0x00, 0x00, 0x00, 0x00};
    } donneesZones;

    
    size_t length = 0;
    int16_t err;

    uint8_t retry = 0;
    do {
        if (!prepareDemand(payload)) return false;
        info("[Satellite %d] Wire target %.1f, ambient %.1f, raw mode 0x%02X (not thermal proof).",
             getNumeroZone(), payload.temperatureConsigne.toFloat(), payload.temperatureAmbiante.toFloat(), payload.mode);
        _writeAttempted = true;
        length = sizeof(donneesZones_t);
        err = this->radio().sendInit(
            this->getId(), 
            ID_CHAUDIERE, 
            this->getIdAssociation(),
            this->incrementIdMessage(),
            0x01, 
            0xA029,
            0x0015,
            0xA02F + (0x0005 * (getNumeroZone() - 1)),
            0x0004,
            (byte*)&payload,
            sizeof(payload),
            (byte*)&donneesZones,
            length,
            _modeVirtuel ? 1 : 6 // Re-check the dead-man before any virtual retry.
        );
        
        if(err != RADIOLIB_ERR_NONE || length != sizeof(donneesZones_t)
            || donneesZones.longueur != sizeof(donneesZones_t) - sizeof(FrisquetRadio::RadioTrameHeader) - 1) {
            delay(30);
            continue;
        }

        setEtatChaudiere(donneesZones.etatChaudiere);
        if (!_modeVirtuel) {
            if(getEtatChaudiere().arretChauffage) _zone.confirmerMode(Zone::MODE_ZONE::HORS_GEL, 0x10);
            else _zone.confirmerMode();
        }
        debug("[SATELLITE Z%d] Retour état chaudière : 0x%02X", getNumeroZone(), donneesZones.etatChaudiere);
        debug("[SATELLITE Z%d] Retour état chaudière : %s", getNumeroZone(), getEtatChaudiere().getLibelle().c_str());
        Date date = donneesZones.date;
        setDate(date);
        
        return true;
    } while(retry++ < 1);

    return false;
}

bool Satellite::prepareDemand(Demand& payload) {
    const bool safe = getEtatChaudiere().arretChauffage || (_modeVirtuel && (_initialReadPending || !_zone.controlCurrent()));
    const MODE mode = safe ? MODE::HORS_GEL : getMode();
    const float ambient = safe ? 30.0f : _zone.getTemperatureAmbiante();
    const float target = safe ? 5.0f : _zone.getTemperatureConsigne();
    if (mode == MODE::INCONNU || !isfinite(ambient) || ambient < 0 || ambient > 50
        || !isfinite(target) || target < 5 || target > 30) return false;
    // Negative 25 C wire error even if the mode byte is misinterpreted.
    // Boiler interpretation/frost behaviour remains a PHYSICAL gate.
    payload.temperatureAmbiante = ambient;
    payload.temperatureConsigne = target;
    payload.mode = encodeModeForSend(mode);
    return true;
}

void Satellite::onAssociationAttempt() {
    _initialReadPending = true;
    _readAttempted = false;
    _writeAttempted = false;
    _consigneAttempted = false;
    _observedZ1.valid = false;
    _modeCompatConnect = false;
    _zone.resetControl();
}

void Satellite::updateObservationAge() {
    if (!_observedZ1.valid) return;
    const uint32_t tick = millis();
    _observedZ1.ageMs += uint32_t(tick - _observedZ1.tick);
    _observedZ1.tick = tick;
}

bool Satellite::onReceive(byte* donnees, size_t length) { 
    if(length < sizeof(FrisquetRadio::RadioTrameHeader)) {
        return false;
    }
    ReadBuffer readBuffer(donnees, length);
    FrisquetRadio::RadioTrameHeader* header = (FrisquetRadio::RadioTrameHeader*) readBuffer.getBytes(sizeof(FrisquetRadio::RadioTrameHeader));

    //info("[SATELLITE Z] Interception envoi consigne.", header->type);
    if(!_modeVirtuel) {

        // Les échanges de consigne de 23 octets ne contiennent pas le programme.
        // Attendre une écriture complète A154 : paramètres (6) + programme (42).
        if(header->type == FrisquetRadio::MessageType::INIT &&
           header->idExpediteur == getId() && !header->isAck() &&
           readBuffer.remainingLength() >= sizeof(FrisquetRadio::RadioTrameInit)) {
            ReadBuffer configurationBuffer = readBuffer;
            FrisquetRadio::RadioTrameInit requete;
            configurationBuffer.getBytes((byte*)&requete, sizeof(requete));
            if(requete.adresseMemoireEcriture.toUInt16() == 0xA154 &&
               requete.tailleMemoireEcriture.toUInt16() == 0x0018 &&
               requete.longueurDonneesEcriture == 48 && configurationBuffer.remainingLength() >= 48) {
                byte parametres[6];
                configurationBuffer.getBytes(parametres, sizeof(parametres));
                Zone::Programmation programmation;
                configurationBuffer.getBytes((byte*)&programmation, sizeof(programmation));
                _zone.setProgrammation(programmation, true);
                _zone.saveConfig();
                info("[SATELLITE Z%d] Programmation hebdomadaire reçue.", getNumeroZone());
                return true;
            }
        }

        if(length == 23 && header->type == FrisquetRadio::MessageType::INIT && header->idExpediteur == this->getId() && header->idMessage != getIdMessage()) { // Réception en écoute
            FrisquetRadio::RadioTrameInit* requete = (FrisquetRadio::RadioTrameInit*) readBuffer.getBytes(sizeof(FrisquetRadio::RadioTrameInit));
            if(requete->adresseMemoireEcriture.toUInt16() == 0xA02F && requete->tailleMemoireEcriture.toUInt16() == 0x0004) { // Envoi consigne

                struct donneesSatellite_t {
                    temperature16 temperatureAmbiante; 
                    temperature16 temperatureConsigne;
                    uint8_t i1 = 0x00; 
                    uint8_t mode = 0x00; // 0x01 Confort, 0x02 Reduit, etc.
                    uint8_t i2[2] = {0};
                }* donneesSatellite = (donneesSatellite_t*) readBuffer.getBytes(sizeof(*donneesSatellite));
                
                //info("[SATELLITE Z%d] Interception envoi consigne.", getNumeroZone());

                uint8_t retry = 0;
                int16_t err;
                
                size_t lengthRx = 0;
                byte buffZones[RADIOLIB_SX126X_MAX_PACKET_LENGTH];

                if(! getEcrasement()) {
                    delay(300);

                    err = radio().receiveExpected(
                        ID_CHAUDIERE,
                        this->getId(),
                        header->idAssociation, 
                        header->idMessage,
                        header->idReception|0x80,
                        header->type,
                        (byte*) buffZones,
                        lengthRx,
                        15, true
                    );
                    
                    if(err != RADIOLIB_ERR_NONE) {
                        return false;
                    }

                    readBuffer = ReadBuffer(buffZones, lengthRx);

                    struct donneesZones_t {
                        FrisquetRadio::RadioTrameHeader header;
                        uint8_t longueur;
                        temperature16 temperatureExterieure;
                        byte i1[2];
                        uint8_t date[6];  // format reçu "YY MM DD hh mm ss"
                        byte etatChaudiere; // mode chaudière à valider
                        uint8_t jourSemaine; // format wday
                        temperature16 temperatureAmbianteZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
                        temperature16 temperatureConsigneZ1;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
                        byte i2 = 0x00;
                        uint8_t modeZ1 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
                        byte i3[4] = {0x00, 0xC6, 0x00, 0xC6};
                        temperature16 temperatureAmbianteZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
                        temperature16 temperatureConsigneZ2;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
                        byte i4 = 0x00;
                        uint8_t modeZ2 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
                        byte i5[4] = {0x00, 0x00, 0x00, 0x00};
                        temperature16 temperatureAmbianteZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
                        temperature16 temperatureConsigneZ3;    // Début 5°C -> 0 = 50 = 5°C - MAX 30°C
                        byte i6 = 0x00;
                        uint8_t modeZ3 = 0x00;                       // 0x05 auto - 0x06 confort - 0x07 reduit - 0x08 hors gel
                        byte i7[4] = {0x00, 0x00, 0x00, 0x00};
                    }* donneesZones = (donneesZones_t*)readBuffer.getBytes(sizeof(donneesZones_t));

                    if(lengthRx < sizeof(donneesZones_t)) {
                        return false;
                    }

                    setEtatChaudiere(donneesZones->etatChaudiere);
                    Date date = donneesZones->date;
                    setDate(date);
                }

                setIdAssociation(header->idAssociation);
                setIdMessage(header->idMessage);
                _zone.setTemperatureAmbiante(donneesSatellite->temperatureAmbiante.toFloat());
                
                if(! getEcrasement()) {
                    const auto modeDemande = _zone.getMode();
                    const byte optionsDemandees = _zone.getModeOptions();
                    const bool commandeConnectEnAttente = _zone.getSource() == Zone::SOURCE::CONNECT
                        && _zone.getLastChange() > _zone.getLastEnvoi();
                    setModeFromRaw(donneesSatellite->mode);
                    if(_zone.getSource() == Zone::SOURCE::SATELLITE_PHYSIQUE) {
                        _zone.desactiverBoost();
                        _zone.confirmerMode();
                    }
                    if(commandeConnectEnAttente) {
                        _zone.setMode(modeDemande);
                        _zone.setModeOptions(optionsDemandees);
                    }
                    _zone.setTemperatureConsigne(donneesSatellite->temperatureConsigne.toFloat());
                    saveConfig();
                    _zone.saveConfig();
                    publishMqtt();
                    _zone.publishMqtt();
                    return true;
                }

                if(getMode() == MODE::INCONNU) {
                    setModeFromRaw(donneesSatellite->mode);
                }
                if(isnan(_zone.getTemperatureConsigne())) {
                    _zone.setTemperatureConsigne(donneesSatellite->temperatureConsigne.toFloat());
                }

                info("[SATELLITE Z%d] Écrasement de données.", getNumeroZone());

                incrementIdMessage(3);

                if(!this->envoyerConsigne()) {
                    error("[SATELLITE Z%d] Échec de l'écrasement.", getNumeroZone());
                } else {
                    info("[SATELLITE Z%d] Écrasement réussie.", getNumeroZone());
                }

                saveConfig();
                _zone.saveConfig();
                publishMqtt();
                _zone.publishMqtt();
                return true;
            }
        }
    } else {
        if(header->idAssociation != getIdAssociation() || header->isAck()) {
            return false;
        }

        //radio().sendAnswer(getId(), header->idExpediteur, header->idAssociation, header->idMessage, header->idReception, header->type, {}, 0);
        return true;
    }

    return false;
}

String Satellite::getNomMode() {
    switch(this->getMode()) {
        case MODE::CONFORT_AUTO:
            return "Auto - Confort";
        case MODE::REDUIT_AUTO:
            return "Auto - Réduit";
        case MODE::CONFORT_PERMANENT:
            return "Confort";
        case MODE::REDUIT_PERMANENT:
            return "Réduit";
        case MODE::CONFORT_DEROGATION:
            return "Confort - Dérog";
        case MODE::REDUIT_DEROGATION:
            return "Réduit - Dérog";
    }

    return "Inconnu";
}

Satellite::MODE Satellite::getMode() {
    switch(_zone.getMode()) {
        case Zone::MODE_ZONE::AUTO :
            if(_zone.confortActif()) {
                return _zone.derogationActive() ? MODE::CONFORT_DEROGATION : MODE::CONFORT_AUTO;
            } else {
                return _zone.derogationActive() ? MODE::REDUIT_DEROGATION : MODE::REDUIT_AUTO;
            }
        case Zone::MODE_ZONE::CONFORT :
            return MODE::CONFORT_PERMANENT;
        case Zone::MODE_ZONE::REDUIT :
            return MODE::REDUIT_PERMANENT;
        case Zone::MODE_ZONE::HORS_GEL :
            return MODE::HORS_GEL;
        default:
            return MODE::INCONNU;
    }
}

void Satellite::setMode(Satellite::MODE mode) { 
    switch(mode) {
        case MODE::CONFORT_AUTO :
            _zone.setMode(Zone::MODE_ZONE::AUTO, true);
            break;
        case MODE::CONFORT_DEROGATION :
            _zone.setMode(Zone::MODE_ZONE::AUTO, true, true);
            break;
        case MODE::REDUIT_AUTO :
            _zone.setMode(Zone::MODE_ZONE::AUTO, false);
            break;
        case MODE::REDUIT_DEROGATION :
            _zone.setMode(Zone::MODE_ZONE::AUTO, false, true);
            break;
        case MODE::CONFORT_PERMANENT :
            _zone.setMode(Zone::MODE_ZONE::CONFORT);
            break;
        case MODE::REDUIT_PERMANENT :
            _zone.setMode(Zone::MODE_ZONE::REDUIT);
            break;
        case MODE::HORS_GEL :
            _zone.setMode(Zone::MODE_ZONE::HORS_GEL);
            break;
    }
}

Satellite::MODE Satellite::decodeModeFromRaw(uint8_t rawMode, bool* compatConnect) const {
    auto decodeDirect = [](uint8_t modeValue) -> MODE {
        switch (modeValue) {
            case MODE::REDUIT_PERMANENT:
            case MODE::CONFORT_PERMANENT:
            case MODE::REDUIT_DEROGATION:
            case MODE::CONFORT_DEROGATION:
            case MODE::REDUIT_AUTO:
            case MODE::CONFORT_AUTO:
            case MODE::HORS_GEL:
                return (MODE)modeValue;
            default:
                return MODE::INCONNU;
        }
    };

    MODE direct = decodeDirect(rawMode);
    if (direct != MODE::INCONNU) {
        if (compatConnect) {
            *compatConnect = false;
        }
        return direct;
    }

    uint8_t xorMode = rawMode ^ MODE_CONNECT_COMPAT_XOR;
    MODE decodedXor = decodeDirect(xorMode);
    if (decodedXor != MODE::INCONNU) {
        if (compatConnect) {
            *compatConnect = true;
        }
        return decodedXor;
    }

    if (compatConnect) {
        *compatConnect = false;
    }
    return MODE::INCONNU;
}

uint8_t Satellite::encodeModeForSend(Satellite::MODE mode) const {
    uint8_t rawMode = (uint8_t)mode;
    if (_modeCompatConnect) {
        rawMode ^= MODE_CONNECT_COMPAT_XOR;
    }
    return rawMode;
}

void Satellite::setModeFromRaw(uint8_t rawMode) {
    bool compatConnect = false;
    MODE decodedMode = decodeModeFromRaw(rawMode, &compatConnect);
    _modeCompatConnect = compatConnect;

    if (decodedMode == MODE::INCONNU) {
        char hexMode[5];
        snprintf(hexMode, sizeof(hexMode), "%02X", rawMode);
        warning(String("[SATELLITE Z") + String(getNumeroZone()) + "] Mode satellite inconnu: 0x" + String(hexMode));
        return;
    }

    if (_modeCompatConnect) {
        debug("[SATELLITE Z%d] Mode avec compatibilité Connect détectée (XOR 0x20).", getNumeroZone());
    }

    setMode(decodedMode);
}
