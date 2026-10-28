#include "FrisquetDevice.h"
#include "../Buffer.h"

bool FrisquetDevice::associer(NetworkID& networkId, uint8_t& idAssociation) {
    onAssociationAttempt();
    const NetworkID previousNetwork = radio().getNetworkID();
    const bool previousInterrupt = FrisquetRadio::interruptReceive;
    FrisquetRadio::interruptReceive = true;
    byte buff[RADIOLIB_SX126X_MAX_PACKET_LENGTH];
    size_t buffLength = 0;
    int16_t err;
    
    const bool listening = radio().setNetworkID({0xFF, 0xFF, 0xFF, 0xFF}) == RADIOLIB_ERR_NONE;
    const uint32_t started = millis();
    bool sent = false;
    NetworkID candidate;
    uint8_t candidateAssociation = 0xFF;
    while (listening && uint32_t(millis() - started) < 30000U) {
        err = radio().receive(buff, 0);
        if(err != RADIOLIB_ERR_NONE) {
            continue;
        }

        buffLength = radio().getPacketLength(); 
        if (uint32_t(millis() - started) >= 30000U
            || !FrisquetRadio::readAssociation(buff, buffLength, getId(), candidate, candidateAssociation)) {
            continue;
        }

        logRadio(true, (byte*)buff, buffLength);
        FrisquetRadio::RadioTrameHeader header;
        memcpy(&header, buff, sizeof(header));
        struct {
            FrisquetRadio::RadioTrameHeader header;
            NetworkID networkID;
        } confirmPayload;
        header.answer(confirmPayload.header);
        confirmPayload.header.idExpediteur = getId();
        confirmPayload.networkID = candidate;
        logRadio(false, (byte*)&confirmPayload, sizeof(confirmPayload));
        for(uint8_t i = 0; i < 10; i++) {
            err = radio().transmit((byte*)&confirmPayload, sizeof(confirmPayload));
            if (err == RADIOLIB_ERR_NONE) sent = true;
            if (err == RadioTxPolicy::INHIBITED || err == RadioTxPolicy::AIRTIME_LIMIT) break;
            delay(30);
        }
        break;
    }
    const bool configured = sent && radio().setNetworkID(candidate) == RADIOLIB_ERR_NONE;
    if (!configured) radio().setNetworkID(previousNetwork);
    FrisquetRadio::receivedFlag = false;
    const int16_t receiveResult = radio().startReceive();
    FrisquetRadio::interruptReceive = previousInterrupt;
    if (!configured || receiveResult != RADIOLIB_ERR_NONE) {
        radio().setNetworkID(previousNetwork);
        radio().startReceive();
        return false;
    }
    networkId = candidate;
    idAssociation = candidateAssociation;
    info("[DEVICE] Confirmation émise localement; réponse chaudière ultérieure non vérifiée.");
    delay(5000); // Preserve the protocol settling interval, not a boiler ACK.
    return true;
}

bool FrisquetDevice::recupererDate() {
    if(! estAssocie()) {
        return false;
    }

    struct {
        FrisquetRadio::RadioTrameHeader header;
        uint8_t longueur;
        byte date[6] = {0};
    } donnees;
    
    size_t length;
    int16_t err;

    uint8_t retry = 0;
    do {
        length = sizeof(donnees);
        err = this->radio().sendAsk(
            this->getId(), 
            ID_CHAUDIERE, 
            this->getIdAssociation(),
            this->incrementIdMessage(),
            0x01,
            0xA02B,
            0x0003,
            (byte*)&donnees,
            length
        );

        if(err != RADIOLIB_ERR_NONE || length < sizeof(donnees)) {
            delay(100);
            continue;
        }

        Date date = donnees.date;
        setDate(date);
        
        return true;
    } while(retry++ < 10);

    return false;
}
