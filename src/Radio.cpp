#include "Radio.h"
#include "Logs.h"

#if defined(RADIO_SX1276)
int16_t Radio::PacketRadio::startReceive() {
    int16_t result = standby();
    if(result != RADIOLIB_ERR_NONE) return result;
    if(getActiveModem() != RADIOLIB_SX127X_FSK_OOK) return RADIOLIB_ERR_WRONG_MODEM;
    result = setCRC(true, false); // CCITT, init 0x1D0F, résultat inversé.
    if(result != RADIOLIB_ERR_NONE) return result;
    // Conserver les paquets CRC invalides pour les compter et les journaliser.
    result = getMod()->SPIsetRegValue(RADIOLIB_SX127X_REG_PACKET_CONFIG_1,
                                     RADIOLIB_SX127X_CRC_AUTOCLEAR_OFF, 3, 3);
    if(result != RADIOLIB_ERR_NONE) return result;
    result = variablePacketLengthMode(MAX_RX_PACKET_LENGTH);
    if(result != RADIOLIB_ERR_NONE) return result;
    return SX1276::startReceive(0, RADIOLIB_IRQ_RX_DEFAULT_FLAGS, RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
}

int16_t Radio::PacketRadio::receive(uint8_t* data, size_t len, RadioLibTime_t timeout) {
    // Même budget que le SX1262 : environ 408 ms si la longueur est inconnue.
    const uint32_t timeoutMs = timeout ? timeout : static_cast<uint32_t>((len ? len : 255) * 8.0f / 25.0f * 5.0f);
    int16_t result = startReceive();
    if(result != RADIOLIB_ERR_NONE) return result;
    const uint32_t started = getMod()->hal->millis();
    while(!getMod()->hal->digitalRead(getMod()->getIrq())) {
        getMod()->hal->yield();
        if(getMod()->hal->millis() - started > timeoutMs) {
            standby();
            clearIrqFlags(RADIOLIB_SX127X_FLAGS_ALL);
            return RADIOLIB_ERR_RX_TIMEOUT;
        }
    }
    return readData(data, len);
}

int16_t Radio::PacketRadio::readData(uint8_t* data, size_t len) {
    lastRxLength = 0;
    lastRxOffset = 0;
    rawLength = 0;
    crcResult = RadioCrc::Result();
    int16_t result = standby();
    if(result != RADIOLIB_ERR_NONE) return result;
    lastRxIrq = getIrqFlags();
    if(lastRxIrq & (RADIOLIB_SX127X_FLAG_FIFO_OVERRUN << 8)) {
        clearIrqFlags(RADIOLIB_SX127X_FLAGS_ALL);
        return RADIOLIB_ERR_CRC_MISMATCH;
    }
    if(!(lastRxIrq & (RADIOLIB_SX127X_FLAG_PAYLOAD_READY << 8))) return RADIOLIB_ERR_RX_TIMEOUT;
    // Dépiler la longueur une seule fois : getPacketLength() pourrait consommer
    // un octet de la trame suivante. Le CRC matériel n'est pas placé en FIFO.
    const uint8_t length = getMod()->SPIreadRegister(RADIOLIB_SX127X_REG_FIFO);
    if(length == 0 || length > MAX_RX_PACKET_LENGTH) {
        clearIrqFlags(RADIOLIB_SX127X_FLAGS_ALL);
        return RADIOLIB_ERR_CRC_MISMATCH;
    }
    raw[0] = length;
    getMod()->SPIreadRegisterBurst(RADIOLIB_SX127X_REG_FIFO, length, raw + 1);
    rawLength = length + 1;
    lastRxLength = length;
    const size_t copied = len && len < length ? len : length;
    memcpy(data, raw + 1, copied);
    const bool crcOk = (lastRxIrq & (RADIOLIB_SX127X_FLAG_CRC_OK << 8)) != 0;
    clearIrqFlags(RADIOLIB_SX127X_FLAGS_ALL);
    return crcOk ? RADIOLIB_ERR_NONE : RADIOLIB_ERR_CRC_MISMATCH;
}
#else
int16_t Radio::PacketRadio::finishTransmit() {
    // Capture before RadioLib clears IRQs. A timeout is never a TX_DONE receipt.
    const uint32_t irq = getIrqFlags();
    txDone = (irq & RADIOLIB_SX126X_IRQ_TX_DONE) && !(irq & RADIOLIB_SX126X_IRQ_TIMEOUT);
    return SX1262::finishTransmit();
}

int16_t Radio::PacketRadio::startReceive() {
    return startReceiveWithLimit(RADIOLIB_SX126X_RX_TIMEOUT_INF);
}

int16_t Radio::PacketRadio::startReceiveWithLimit(uint32_t timeout) {
    // RadioLib 7 conserve la longueur GFSK fixe lors de l'armement RX.
    int16_t result = standby();
    if (result != RADIOLIB_ERR_NONE) return result;
    if (getPacketType() != RADIOLIB_SX126X_PACKET_TYPE_GFSK) return RADIOLIB_ERR_WRONG_MODEM;
    // Désactiver le traitement matériel pour conserver longueur et CRC reçus.
    // Réappliquer explicitement la longueur brute avant chaque réception.
    result = setCRC(0);
    if (result != RADIOLIB_ERR_NONE) return result;
    result = fixedPacketLengthMode(RadioCrc::RAW_LENGTH);
    if (result != RADIOLIB_ERR_NONE) return result;
    return SX1262::startReceive(timeout, RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
                               RADIOLIB_IRQ_RX_DEFAULT_MASK, RadioCrc::RAW_LENGTH);
}

int16_t Radio::PacketRadio::receive(uint8_t* data, size_t len, RadioLibTime_t timeout) {
    // Conserver les délais de RadioLib à 25 kbit/s (408 ms pour len == 0),
    // La capture brute dure toujours 68 octets, même pour une réponse courte.
    const size_t expectedLength = len == 0 ? RADIOLIB_SX126X_MAX_PACKET_LENGTH : RadioCrc::RAW_LENGTH;
    const uint32_t timeoutMs = timeout != 0 ? timeout
        : static_cast<uint32_t>(expectedLength * 8.0f / 25.0f * 5.0f);
    const uint32_t radioTimeout = static_cast<uint32_t>(timeoutMs * 1000.0f / 15.625f);
    int16_t result = startReceiveWithLimit(radioTimeout);
    if (result != RADIOLIB_ERR_NONE) return result;
    const uint32_t started = getMod()->hal->millis();
    bool softTimeout = false;
    while (!getMod()->hal->digitalRead(getMod()->getIrq())) {
        getMod()->hal->yield();
        if (getMod()->hal->millis() - started > timeoutMs) {
            softTimeout = true;
            break;
        }
    }
    result = standby();
    if (result != RADIOLIB_ERR_NONE && result != RADIOLIB_ERR_SPI_CMD_TIMEOUT) return result;
    if (softTimeout || (getIrqFlags() & RADIOLIB_SX126X_IRQ_TIMEOUT)) {
        clearIrqStatus();
        return RADIOLIB_ERR_RX_TIMEOUT;
    }
    return readData(data, len);
}

int16_t Radio::PacketRadio::readData(uint8_t* data, size_t len) {
    // Stabiliser le paquet et ses métadonnées pendant la copie. Le réarmement
    // reste à la charge de l'appelant, comme pour receive() bloquant.
    lastRxLength = 0;
    lastRxOffset = 0;
    rawLength = 0;
    crcResult = RadioCrc::Result();
    int16_t result = standby();
    if (result != RADIOLIB_ERR_NONE) return result;
    lastRxIrq = getIrqFlags();
    if (!(lastRxIrq & (RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR
                       | RADIOLIB_SX126X_IRQ_HEADER_ERR))) {
        return RADIOLIB_ERR_RX_TIMEOUT;
    }

    // En RX continu, les paquets successifs peuvent avoir des offsets différents.
    // Lire la capture brute depuis l'offset réellement signalé par le SX1262.
    uint8_t status[2] = {0, 0};
    result = getMod()->SPIreadStream(RADIOLIB_SX126X_CMD_GET_RX_BUFFER_STATUS, status, 2);
    if (result != RADIOLIB_ERR_NONE) return result;
    lastRxOffset = status[1];
    rawLength = status[0] < sizeof(raw) ? status[0] : sizeof(raw);
    result = readBuffer(raw, static_cast<uint8_t>(rawLength), lastRxOffset);
    if (result != RADIOLIB_ERR_NONE) return result;
    result = clearIrqStatus();
    if (result != RADIOLIB_ERR_NONE) return result;
    // Décoder dans un tampon local : ne pas dépasser len lors de la copie.
    uint8_t payload[MAX_RX_PACKET_LENGTH];
    if (status[0] == sizeof(raw) && (lastRxIrq & RADIOLIB_SX126X_IRQ_RX_DONE)
        && !(lastRxIrq & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR))) {
        crcResult = RadioCrc::decode(raw, rawLength, payload);
    }
    lastRxLength = crcResult.length;
    const bool accepted = crcResult.status == RadioCrc::Status::Valid
        || crcResult.status == RadioCrc::Status::Repaired;
    const size_t capturedLength = len != 0 && len < lastRxLength ? len : lastRxLength;
    if (capturedLength) memcpy(data, accepted ? payload : raw + 1, capturedLength);
    return accepted ? RADIOLIB_ERR_NONE : RADIOLIB_ERR_CRC_MISMATCH;
}

#endif

bool Radio::persistTxBudget() {
    Preferences preferences;
    if (!preferences.begin("ofvRfBudget", false)) return false;
    uint32_t record[] = {_txPolicy.usedUs(), _txPolicy.usedUs() ^ 0x52464232U};
    const bool saved = preferences.putBytes("used", record, sizeof(record)) == sizeof(record);
    preferences.end();
    return saved;
}

bool Radio::loadTxBudget() {
    Preferences preferences;
    if (!preferences.begin("ofvRfBudget", false)) return false;
    uint32_t record[2] = {};
    const bool valid = preferences.getBytesLength("used") == sizeof(record)
        && preferences.getBytes("used", record, sizeof(record)) == sizeof(record)
        && record[0] <= RadioTxPolicy::BUDGET_US && record[1] == (record[0] ^ 0x52464232U);
    preferences.end();
    // No trusted RTC: retain ALL recovered spend for a new conservative window.
    // Absent/corrupt legacy ledger is UNKNOWN, not a free refilled budget.
    _txPolicy.reset(millis(), valid ? record[0] : RadioTxPolicy::BUDGET_US);
    return persistTxBudget();
}

int16_t Radio::transmit(byte data[], size_t len) {
    if(len == 0 || len > MAX_RX_PACKET_LENGTH) return RADIOLIB_ERR_PACKET_TOO_LONG;
    if (!_txReady) return RadioTxPolicy::INHIBITED;

    // Restaurer le format sur l'air : longueur variable et CRC matériel.
    int16_t result = _radio.standby();
    if (result != RADIOLIB_ERR_NONE) return result;
#if defined(RADIO_SX1276)
    result = _radio.setCRC(true, false);
#else
    result = _radio.setCRC(2, 0x1D0F, 0x1021, true);
#endif
    if (result != RADIOLIB_ERR_NONE) return result;
    result = _radio.variablePacketLengthMode(MAX_RX_PACKET_LENGTH);
    if (result != RADIOLIB_ERR_NONE) return result;
    // getTimeOnAir is microseconds. RadioLib omits the variable length octet.
    const uint32_t nominalUs = _radio.getTimeOnAir(len + 1);
    const uint32_t expectedUs = uint32_t((len + 9U) * 320U);
    if (nominalUs + 1 < expectedUs || nominalUs > expectedUs + 1) return RadioTxPolicy::INHIBITED;
    if (!_txPolicy.reserve(millis(), len, RadioTxPolicy::isNoHeat(data, len))) return RadioTxPolicy::AIRTIME_LIMIT;
    // NVS commits the full ambiguous-TX bound BEFORE any packet can leave.
    if (!persistTxBudget()) return RadioTxPolicy::INHIBITED;
#if !defined(RADIO_SX1276)
    _radio.txDone = false;
#endif
    const uint32_t started = millis();
    result = _radio.transmit(data, len);
    const uint32_t elapsedMs = uint32_t(millis() - started);
#if !defined(RADIO_SX1276)
    const bool confirmed = result == RADIOLIB_ERR_NONE && _radio.txDone;
    if (result == RADIOLIB_ERR_NONE && !confirmed) result = RADIOLIB_ERR_TX_TIMEOUT;
#else
    const bool confirmed = false; // No newly qualified sibling profile.
#endif
    if (confirmed && elapsedMs <= RadioTxPolicy::chargeUs(len) / 1000U) {
        const uint32_t packetBound = nominalUs + (nominalUs + 99U) / 100U + 2000U;
        const uint32_t elapsedBound = elapsedMs * 1000U + 1000U;
        _txPolicy.confirm(packetBound > elapsedBound ? packetBound : elapsedBound);
        // A failed reduction leaves the larger reservation on flash, never a refund.
        if (!persistTxBudget()) return RadioTxPolicy::INHIBITED;
    }
    if (elapsedMs > RadioTxPolicy::chargeUs(len) / 1000U) {
        _txReady = false;
        error("[RADIO] TX timing fault; further TX inhibited, no airtime refund.");
    }
    return result;
}

int16_t Radio::receive(uint8_t data[], size_t len) {
    _radio.lastRxIrq = 0;
    _radio.lastRxLength = 0;
    _radio.lastRxOffset = 0;
    return finishReceive(_radio.receive(data, len), "receive", data, len);
}

int16_t Radio::readData(uint8_t data[], size_t len) {
    _radio.lastRxIrq = 0;
    _radio.lastRxLength = 0;
    _radio.lastRxOffset = 0;
    // Le contenu du tampon survit à sa lecture, contrairement aux IRQ. Sans
    // nouvel événement RX, le relire pourrait valider un ancien paquet CRC.
    if (!hasReceivedPacket()) {
        _lastPacketRSSI = NAN;
        return RADIOLIB_ERR_RX_TIMEOUT;
    }
    return finishReceive(_radio.readData(data, len), "readData", data, len);
}

bool Radio::hasReceivedPacket() {
    const uint16_t irq = _radio.getIrqFlags();
#if defined(RADIO_SX1276)
    return (irq & ((RADIOLIB_SX127X_FLAG_PAYLOAD_READY | RADIOLIB_SX127X_FLAG_FIFO_OVERRUN) << 8)) != 0;
#else
    return (irq & (RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR
                   | RADIOLIB_SX126X_IRQ_HEADER_ERR)) != 0;
#endif
}

int16_t Radio::finishReceive(int16_t result, const char* operation, const uint8_t* data, size_t requestedLength) {
    // Relever le RSSI avant le prochain réarmement.
    const bool packetRead = result == RADIOLIB_ERR_NONE || result == RADIOLIB_ERR_CRC_MISMATCH;
    _lastPacketRSSI = packetRead ? _radio.getRSSI() : NAN;
#if defined(RADIO_SX1276)
    if(result == RADIOLIB_ERR_CRC_MISMATCH) {
        logs.addLogf("RADIO", "[RX][CRC_ERROR][mode=hardware][chip=SX1276][len=%u][IRQ=0x%04X][RSSI=%.1f dBm]",
                     static_cast<unsigned>(_radio.lastRxLength), static_cast<unsigned>(_radio.lastRxIrq), _lastPacketRSSI);
        logRadioCrcData(_radio.raw + 1, _radio.lastRxLength, "CRC_DATA");
    }
#else
    if (result == RADIOLIB_ERR_NONE && _radio.crcResult.status == RadioCrc::Status::Repaired) {
        logs.addLogf("RADIO", "[RX][CRC_REPAIRED][len=%u][bit=%u][zero=%+d][edits=%u][RSSI=%.1f dBm]",
                     static_cast<unsigned>(_radio.lastRxLength),
                     static_cast<unsigned>(_radio.crcResult.bit),
                     static_cast<int>(_radio.crcResult.adjustment),
                     static_cast<unsigned>(_radio.crcResult.correctedBits), _lastPacketRSSI);
        logRadioCrcData(_radio.raw, _radio.rawLength, "CRC_RAW");
    }
    if (result == RADIOLIB_ERR_CRC_MISMATCH) {
        const size_t packetLength = _radio.lastRxLength;
        const size_t capturedLength = requestedLength != 0 && requestedLength < packetLength
            ? requestedLength : packetLength;
        const uint16_t irq = _radio.lastRxIrq;
        logs.addLogf("RADIO", "[RX][CRC_ERROR][code=%d][len=%u][captured=%u][RSSI=%.1f dBm][via=%s][IRQ=0x%04X][RX_DONE=%u][CRC_ERR=%u][HEADER_ERR=%u][RX_OFFSET=%u]",
                     result, static_cast<unsigned>(packetLength), static_cast<unsigned>(capturedLength),
                     _lastPacketRSSI, operation, static_cast<unsigned>(irq),
                     (irq & RADIOLIB_SX126X_IRQ_RX_DONE) ? 1U : 0U,
                     (irq & RADIOLIB_SX126X_IRQ_CRC_ERR) ? 1U : 0U,
                     (irq & RADIOLIB_SX126X_IRQ_HEADER_ERR) ? 1U : 0U,
                     static_cast<unsigned>(_radio.lastRxOffset));
        logRadioCrcData(data, capturedLength);
        logs.addLogf("RADIO", "[RX][CRC_CHECK][mode=software][status=%s][rawlen=%u]",
                     RadioCrc::statusName(_radio.crcResult.status), static_cast<unsigned>(_radio.rawLength));
        logRadioCrcData(_radio.raw, _radio.rawLength, "CRC_RAW");
    }
#endif
    return result;
}

#if defined(WIFI_LORA_32_V4) && defined(USE_KCT8103L_PA)
namespace {
constexpr uint8_t KCT8103L_POWER_PIN = GPIO_NUM_7;
constexpr uint8_t KCT8103L_ENABLE_PIN = GPIO_NUM_2;
constexpr uint8_t KCT8103L_CTX_PIN = GPIO_NUM_5;
}
#endif

Radio::Radio() : _radio(new Module(RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO)) {}

void Radio::init(float rxBandwidth, bool lnaBypass) {
    _txReady = false;
    const bool budgetReady = loadTxBudget();
#if defined(RADIO_SX1276)
    SPI.begin(RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);
#endif
#if defined(WIFI_LORA_32_V4) && defined(USE_KCT8103L_PA)
    // Heltec WiFi LoRa 32 V4.3: power and enable the KCT8103L FEM.
    pinMode(KCT8103L_POWER_PIN, OUTPUT);
    digitalWrite(KCT8103L_POWER_PIN, HIGH);
    pinMode(KCT8103L_ENABLE_PIN, OUTPUT);
    digitalWrite(KCT8103L_ENABLE_PIN, HIGH);

    // PA_CTX high selects RX bypass when DIO2 is low, and TX when DIO2 is
    // high. beginFSK() configures the SX1262 to control DIO2 automatically.
    const uint32_t rfSwitchPins[Module::RFSWITCH_MAX_PINS] = {
        KCT8103L_CTX_PIN, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
    };
    // RadioLib keeps a reference to this table: it must outlive init().
    static const Module::RfSwitchMode_t rfSwitchTable[] = {
        { Module::MODE_IDLE, { LOW } },
        { Module::MODE_RX,   { HIGH } },
        { Module::MODE_TX,   { HIGH } },
        END_OF_MODE_TABLE,
    };
    if (lnaBypass) {
        _radio.setRfSwitchTable(rfSwitchPins, rfSwitchTable);
    } else {
        // RX : GPIO5 LOW (LNA actif). TX : GPIO5 HIGH (PA actif).
        _radio.setRfSwitchPins(RADIOLIB_NC, KCT8103L_CTX_PIN);
    }
    info("[RADIO] KCT8103L : bypass du LNA %s en réception.", lnaBypass ? "activé" : "désactivé");
#endif

    if (!RadioSettings::validRxBandwidth(rxBandwidth)) rxBandwidth = RadioSettings::DEFAULT_RX_BANDWIDTH;
    const int16_t initResult = _radio.beginFSK(868.96f, 25.0f, 50.0f, rxBandwidth,
                                             RadioTxPolicy::CHIP_POWER_DBM, 16);
    if(initResult != RADIOLIB_ERR_NONE) {
        error("[RADIO] Échec initialisation FSK : %d", initResult);
        return;
    }

    const int16_t bandwidthResult = _radio.setRxBandwidth(rxBandwidth);
    if (bandwidthResult != RADIOLIB_ERR_NONE) {
        error("[RADIO] Échec configuration bande RX %.1f kHz : %d", rxBandwidth, bandwidthResult);
    } else {
        info("[RADIO] Bande RX configurée : %.1f kHz.", rxBandwidth);
    }
    const int16_t preambleResult = _radio.setPreambleLength(16);
#if defined(OFV_HELTEC_V43_LOW_POWER_PROFILE) && defined(WIFI_LORA_32_V4) && defined(USE_KCT8103L_PA) && !defined(RADIO_SX1276)
    _txReady = budgetReady && bandwidthResult == RADIOLIB_ERR_NONE && preambleResult == RADIOLIB_ERR_NONE
        && RadioTxPolicy::HELTEC_V43_ENVELOPE_QUALIFIED;
#endif
    (void)budgetReady; (void)preambleResult;
    if (!_txReady) error("[RADIO] TX INHIBITED: missing installed-chain tolerance bound (drive error + pad/mismatch + FEM supply/temperature); nominal net gain is NOT raw FEM gain. Chip=%d dBm, 3.6 s/hour incl 0.5 s no-heat reserve, NVS budget recovery.", RadioTxPolicy::CHIP_POWER_DBM);
#if defined(RADIO_SX1276)
    info("[RADIO] SX1276 : longueur variable, %u octets max, CRC matériel CCITT.", MAX_RX_PACKET_LENGTH);
#else
    info("[RADIO] RX brut : %u octets, données : %u max, CRC logiciel et réparation unique.",
         static_cast<unsigned>(RadioCrc::RAW_LENGTH), MAX_RX_PACKET_LENGTH);
#endif
}
