# 🧠 OpenFrisquetVisio

## Candidat de préparation V4.3 — émission interdite

Ce candidat local n'est pas une mise en service. L'unique profil envisagé est
`heltec_wifi_lora_32_V4_3`, SX1262 à −9 dBm demandés, 868,96 MHz, GFSK 25 kbit/s,
FEM KCT8103L et antenne 863–928 MHz de gain ≤4 dBi. Les autres cibles ne sont
pas qualifiées. `HELTEC_V43_ENVELOPE_QUALIFIED` reste faux, sans dérogation runtime :
le prédicat de tolérance ci-dessous manque encore. Aucun essai RF/thermique,
aucune réception physique ou conformité mesurée n'est revendiqué.

### Puissance : chaîne complète, pas gain FEM brut

Le [schéma officiel V4.3](https://resource.heltec.cn/download/WiFi_LoRa_32_V4/Schematic/HTIT-WB32LAF_V4.3.pdf)
montre RFO → pad π (59 Ω shunt / 280 Ω série / 59 Ω shunt) → TX **pin 1** du
KCT8103L ; TX_IN **pin 11** est après le PA. Le pad nominal vaut 21,141248 dB
en référence 50 Ω. Vfem vient du TLV75733 (nominal 3,3 V). La
[fiche KCT8103L Rev A](https://resource.heltec.cn/download/HT-RA62A/KCT8103L_DS_REVA_20241010.pdf)
donne 36 dB de gain petit signal maximum à 3,3 V et **25 °C**, ports adaptés.
Son tableau de contrôle confirme CSD/CTX/CPS = 1/1/1 en TX, 1/0/X en RX LNA,
1/1/0 en RX bypass, 0/X/X en shutdown ; PA_CPS vient de DIO2, PA_CTX de GPIO5,
PA_CSD de GPIO2. Aucune modification de ce chemin matériel n'est nécessaire.

Avec puissance puce nominale et antenne ≤4 dBi, le calcul à 25 °C est :
`−9 − 21,141248 + 36 + 4 − 2,15 = 7,708752 dBm ERP`.
Le plafond 25 mW ERP vaut 13,979400 dBm : il reste **6,270648 dB** pour l'erreur
de puissance réelle de la puce, le pad/son désadaptement et la variation de gain
FEM avec alimentation/température. C'est le **majorant conjoint** de ces termes
qui manque, pas une impossibilité matérielle. Le schéma ne spécifie pas la
tolérance des résistances ; supposer ±5 % donne 20,418532 dB de pad minimal,
mais reste une hypothèse, pas une propriété prouvée du kit. La table FEM à
25 °C et un article de portée ne bornent pas ce majorant sur les conditions
réelles. Une justification primaire de ce majorant suffit à réexaminer le gate ;
ce constat n'impose ni achat ni mesureur particulier.

L'[article Heltec V4.3](https://wiki.heltec.org/news/how-far-can-heltec-wifi-loRa-32-v4-reach-r2-vs-r8-tested-in-the-real-world/how-far-can-heltec-wifi-loRa-32-v4-reach-r2-vs-r8-tested-in-the-real-world)
corrobore environ 11 dB de gain **net** à 923,2 MHz/LoRa, pas un maximum garanti
à 868,96 MHz/GFSK. La [page antennes Heltec](https://heltec.org/project/sma-antenna/)
donne whip 4 dBi et glue rod 3 dBi ; l'identité de l'antenne installée reste à
confirmer avant admission. RadioLib appelle déjà `setOutputPower` depuis
`beginFSK` ; sa table optimisée traduit la demande −9 en paVal −5 / duty 2 /
hpMax 2. Ni ce registre ni −9 ne constituent une mesure à l'antenne.

### Temps d'antenne et redémarrage

Toutes les tentatives passent par `Radio::transmit`, association et retries inclus.
Un seul budget de **3 600 000 µs** couvre l'heure glissante. Les 62 tranches
d'une minute retiennent aussi un paquet chevauchant une frontière : récupération
conservatrice après 61–62 minutes, jamais créditée par l'horloge civile.
Les 500 000 µs de réserve non-chauffante sont **dans** ce budget. Le trafic
ordinaire ne peut pas les dépenser. Seul le payload Z1 exact 30 °C / 5 °C /
mode brut 0x10 / options nulles y accède ; ni une priorité d'API ni un second
budget d'association ne permettent de contourner cette règle.

Avant TX, Preferences/NVS `ofvRfBudget/used` engage le timeout complet :
`6 000 µs + 5 × temps nominal`. RadioLib exprime FSKRate_t en **kbit/s**, le
préambule en **bits** et `getTimeOnAir` en **µs**. Le code ajoute l'octet de
longueur que son calcul GFSK omet et vérifie le format effectif. Après TX_DONE
vérifié **et** retour réussi de standby, seulement, la réservation peut être
réduite au maximum de (temps nominal + 1 % + 2 ms) et (durée bloquante observée
+ 1 ms). Erreur/IRQ ambiguë : aucun remboursement. Dépassement du timeout
comptabilisé : aucune réduction et inhibition des émissions suivantes.

Un reboot recharge toute la dépense NVS comme récente, sans la remettre à zéro.
Après du trafic ordinaire, la réserve reste donc disponible immédiatement,
même au reboot ; le test admet 8 tentatives Z1 ambiguës après saturation du
trafic ordinaire. Un ledger absent/corrompu signifie **budget antérieur inconnu** :
aucune émission pendant la fenêtre de récupération. Ce cas de migration doit
être traité **avant** association/chauffe, pas présenté comme un repli en marche.
Une panne NVS empêche un nouvel engagement et donc le TX. Aucun budget fini
ne peut garantir réception ou coupure en cas de panne radio/stockage répétée.

Le test exécute la boucle Satellite avec 60 heartbeats, 26 lectures (cadence
native plus 12 lectures CDC supplémentaires) et un repli : 87 tentatives
réussies simulées, comptées 964 589 µs, sous le plafond ordinaire de 3,1 s.
Ce n'est ni une mesure de waveform ni une garantie de temps de réponse physique.

Contrôle virtuel, sans changer les topics scalaires existants :

1. Base par défaut `frisquet`. Le producteur souscrit `<baseTopic>/controlSession`
   (état retained, 16 caractères hexadécimaux), renouvelé à chaque connexion
   MQTT **du firmware**, y compris après boot. Le défi n'est ni une identité
   ni une autorisation. La disponibilité est
   `<baseTopic>/openFrisquetVisio/availability` (`online`/`offline`) ; cet état
   retained n'est pas une preuve de connexion actuelle (pas de Last Will ici).
2. Toutes les 60 s, il recalcule les données courantes et publie sans retain
   `z1/mode/set`, `z1/temperatureConsigne/set`, `z1/temperatureAmbiante/set`
   (ordre libre), puis `z1/controlHeartbeat/set` = `<défi>:<séquence uint64>`.
   Ces quatre commandes partent sur une même connexion producteur, sans autre
   writer concurrent, avec le heartbeat **après** les trois scalaires. La
   séquence est décimale, uint64 sans signe (0..18446744073709551615), et doit
   croître strictement dans cette session. Chaque heartbeat
   exige les trois commandes valides reçues depuis le heartbeat précédent,
   chacune âgée d'au plus 60 s. Les profils seuls ne qualifient rien.
3. Le bail expire exactement à 1 800 000 ms depuis le dernier heartbeat accepté,
   indépendamment des mises à jour scalaires suivantes et de l'horloge civile.
   Boot/restauration NVS, perte MQTT/Wi-Fi, reconnexion et tentative de réassociation
   désarment le contrôle ; il faut de nouvelles données et un nouveau heartbeat.
   Les temps d'origine de réception ne sont jamais restaurés comme « maintenant ».
   Sur nouveau défi, abandonner l'ancien cycle, vider les commandes en attente,
   recommencer les trois scalaires puis la séquence de la nouvelle session.
   Si seul le producteur redémarre et que le défi reste identique, il doit
   préserver sa séquence croissante : repartir arbitrairement à 1 sera rejeté.
   Il n'existe pas de topic d'ACK de séquence ; les états scalaires ne sont pas
   un ACK. `z1/controlSafety` est une éligibilité locale, pas une réception chaudière.
   Ce protocole atteste un producteur actif ; il ne mesure pas l'âge physique
   du capteur, que le producteur doit qualifier avant de publier. Un producteur
   en transition-only ne respecte PAS ce contrat, même si ses topics scalaires
   étaient compatibles avant le bail. Ce correctif ne migre aucun writer HA/NAS.

Sans bail valide, ou si la chaudière déclare l'arrêt chauffage, le payload est
fixé à consigne 5 °C / ambiance de contrôle 30 °C / mode Hors Gel (erreur −25 °C).
Ce n'est pas une température ambiante mesurée ni le secours NAS à 20 °C.
Le sens réel du mode et la réaction chaudière restent à vérifier physiquement.
La boucle tente ce sens non-chauffant, pas seulement l'arrêt des transmissions.
L'inhibition RF prime TOUJOURS, même sur ce payload.

Une lecture pure A029 complète et correspondant à l'association précède toute
commande chauffante. Si elle échoue, elle reste en attente et seule la direction
non-chauffante est tentée ; elle peut avoir modifié l'état avant une lecture
ultérieure. `z1/boilerObserved` expose ambiance, consigne, mode BRUT, âge et
`beforeWriteAttempt` pour ne pas présenter une telle lecture comme l'état initial.
Il ne télécharge PAS les trois presets Confort/Réduit/Hors Gel ou le programme,
et ne modifie ni la Zone désirée ni le mode de compatibilité Connect.
`z1/controlSafety` distingue le contrôle courant de la direction de sécurité,
en rappelant que RF reste inhibé. Les états scalaires historiques restent désirés,
pas des reçus radio/thermiques.

L'association ne réussit localement que si une confirmation est émise ; son
retour HTTP précise `boilerResponseVerified=false`. Aucun ACK final chaudière
inexistant n'est inventé. L'échec restaure le réseau RX précédent, sans persister
de nouveaux identifiants. Une réassociation réarme lecture et qualification.

Avant installation/émission : revue indépendante de ce head, mapping de puissance
HP/antenne et budget RF effectif, producteur adoptant le protocole ci-dessus,
retour au satellite physique et interprétation non-chauffante qualifiés, ainsi
que préconditions hydrauliques. Le writer HA existant n'est pas migré par ce
correctif. Pas de nouveau contrôleur, horaires copiés, activation Connect ou ECS.

Vérifications logicielles : `python3 tests/test_commissioning.py`, les autres
`tests/test_*.py`, et `node --test tests/*.test.mjs`. Les doubles ne prouvent
ni réception RF ni effet thermique.

Emulation complète du module **Frisquet Connect**, **sonde Extérieure** ou encore **satellite d'ambiance** sur base **Heltec ESP32 WiFi LoRa 32 (SX1262)** ou **LilyGo/TTGO LoRa32 V2.1 (SX1276, 868 MHz)**.
Ce projet permet à un ESP32 de dialoguer directement avec votre chaudière Frisquet,  
de récupérer les informations en temps réel et de les exposer à **Home Assistant via MQTT**.

---

## 🚀 Fonctionnalités principales

### 🔧 Emulation Frisquet Connect
- Récupération des informations chaudière :
  - Température **ECS (eau chaude sanitaire)**
  - Température **corps de chauffe (CDC)**
  - Température **extérieure**
  - **Pression** du circuit (bar)
  - **Consommation gaz** ECS et chauffage (veille, et cumul de l'année en cours utilisable dans le tableau de bord Énergie de Home Assistant)
  - **Affichage et séléction du mode ECS** (Max, Eco, Eco Horaires Eco+, Eco+ Horaires, Stop)
- **Mode Connect passif** : écoute des échanges chaudière sans émettre de trame (utile si une box Connect est déjà installée).
- Gestion des **zones 1, 2 et 3** :
  - Température **départ eau**
  - Température **ambiante** (si source Connect)
  - Température **consigne**
  - **Configuration des consignes** pour chaque mode :
    - Réduit
    - Hors gel
    - Confort
  - **Sélection du mode actif** (Auto, Confort, Réduit, Hors Gel)

### 🌡️ Emulation de la sonde extérieure
Deux modes possibles :
- Lecture **réelle** via une sonde **DS18B20**
- Lecture **virtuelle** via **MQTT**, permettant d’utiliser la température issue de la météo via un capteur HA.

### Gestion des satellites (utile si chaudière non-compatible Connect)
- Gestion des **zones 1, 2 et 3** :
  - Température **ambiance**
  - Température **consigne**
  - **Configuration d'un boost consigne** (écrasement de la consigne envoyé par le satellite)
  - **Affichage du mode actif** (Auto, Confort, Réduit, Hors Gel)

### 🧩 Intégration Home Assistant (MQTT Discovery)
- Découverte automatique de tous les capteurs et entités :
  - Capteurs de température, de consommation, d’état de zones
  - Sélecteurs de modes
  - Commandes de consigne et interrupteurs (Boost, associations, etc.)
- Entités disponibles immédiatement dans Home Assistant
- **État chaudière** regroupe l'activité : Chauffage, Chauffage ECS ou Veille (Inconnu si les données manquent). Cet état utilise uniquement les indicateurs de fonctionnement et le mode reçus par radio. **Anomalie chaudière** reste un capteur binaire indépendant ; une anomalie ne remplace plus l'activité affichée. L'ancien capteur « Mode de fonctionnement » est retiré de la découverte MQTT. Adapter les automatisations qui utilisaient ce capteur ou l'état « Fonctionnement ».

- Le thermostat propose **Auto**, **Dérogation Confort**, **Dérogation Réduit**, **Confort**, **Réduit**, **Hors Gel** et **Boost**. Auto et ses dérogations sont affichés en mode HVAC `auto` ; les régimes permanents et Boost en `heat`. Hors Gel conserve la protection et ne correspond pas à un arrêt.
- Le capteur **Mode actif** conserve le détail du mode, y compris en écoute seule. Le capteur **Régime actif** indique Confort, Réduit ou Hors Gel. Boost applique une seule fois le supplément configuré à la consigne Confort, dans la limite de 30 °C. Modifier la consigne du thermostat ajuste le profil actif ; en Boost, la consigne affichée inclut déjà le supplément.
- Pour un satellite physique, sans Connect actif ni écrasement radio activé, le thermostat reste en lecture seule. Pour un satellite physique ou le Connect, un mode demandé est publié après retour radio ou réponse réussie ; le satellite virtuel publie son état local.
- Avec un satellite virtuel, définir le programme depuis **Programmation** (`/programming`). Auto nécessite un programme connu et l'heure chaudière synchronisée. Les dérogations locales prennent fin au prochain changement Confort/Réduit ; un programme constant ne permet pas de dérogation temporaire. Leur échéance est conservée au redémarrage. Choisir Auto ou modifier le programme local annule la dérogation. Avec le Connect, la gestion du programme et de la fin de dérogation reste confiée au système distant.
- Les topics `zN/mode/set` acceptent les commandes `Confort` et `Réduit`, ainsi que les anciens noms `Confort permanent` et `Réduit permanent` pour compatibilité. Les états publiés utilisent les libellés `Confort` et `Réduit`.

### 🌐 Portail Web intégré
- Configuration du **WiFi** et du **MQTT**
- **Programmation hebdomadaire** : créneaux Confort/Réduit de 30 minutes, copie de journées et sauvegarde persistante pour les zones pilotées par un Connect actif ou un satellite virtuel. Accessible depuis le tableau de bord ou `/programming`.
- Visualisation des **logs**
- Informations système et réseau
- Envoi de trame radio personnalisée (debug)
- Lecture de zones mémoire (debug)

Les pages du portail se trouvent dans le dossier **`web/`**. Lors d'une compilation
PlatformIO, `tools/generate_web_assets.py` les transforme automatiquement en
ressources C++ stockées dans le firmware.

La programmation est envoyée avec le bloc complet de configuration de la zone au
prochain cycle du Connect. Son utilisation nécessite le mode Auto ; enregistrer
la semaine ne change pas le mode de chauffage. Les zones en écoute passive ou
pilotées par un satellite virtuel sont affichées en lecture seule.

Les tests du codage des demi-heures et de la sauvegarde se lancent avec
`node --test tests/programming.test.mjs` et
`python3 tests/test_programming_state.py` (compilateur C++ requis).

### 🔁 Mise à jour OTA
- Mise à jour du firmware directement via WiFi

---

## 🧰 Matériel nécessaire

| Composant | Description | Remarques |
|------------|-------------|-----------|
| 🧠 Heltec ESP32 WiFi LoRa 32 V3/V4 (SX1262) | Carte principale | Module **SX1262** |
| 🧠 LilyGo/TTGO LoRa32 V2.1 (SX1276) | Alternative | Version **868 MHz** |
| 🌡️ DS18B20 (optionnel) | Sonde de température | GPIO **33** sur Heltec, GPIO **13** sur LilyGo |
| 🔩 Résistance 4,7 kΩ | Pull-up pour DS18B20 | Entre **VCC** et **DATA** |
| ☕ Café | Indispensable | Pour le développeur 😁 |

---

## ⚙️ Configuration et installation

### 1️⃣ Préparation du firmware

Avant le flash :
- Ouvrir le fichier **`include/Board.h`**
  et vérifier / modifier les options selon vos besoins :

| Option | Description |
|---------|-------------|
| `DS18B20_PIN` | GPIO utilisé (33 sur Heltec, 13 sur LilyGo) |

---

### 2️⃣ Flash du firmware

- Connecter la carte via **USB**
- Compiler et téléverser avec **PlatformIO**
- Au premier démarrage, le module crée un **point d’accès WiFi**

---

### Variante LilyGo/TTGO LoRa32 V2.1 (SX1276)

Le support reprend le brochage de la [PR #20 de Julien Hammer](https://github.com/FreedomNX/OpenFrisquetVisio/pull/20), adapté à RadioLib 7.8.1 et à la gestion actuelle de réception. Il cible la LoRa32 V2.1 avec **SX1276 en 868 MHz** ; les autres cartes LilyGo peuvent avoir un brochage différent.

La configuration [platformio.lilygo.ini](platformio.lilygo.ini) importe les réglages communs de votre `platformio.ini` local et remplace la carte, les dépendances Heltec et le mode de téléversement. Elle utilise un partitionnement avec OTA et un premier flash USB :

```sh
pio run --project-conf platformio.lilygo.ini -e ttgo_lora32_v21
pio run --project-conf platformio.lilygo.ini -e ttgo_lora32_v21 -t upload
```

| Signal | GPIO LilyGo |
|---|---|
| SPI SCK / MISO / MOSI | 5 / 19 / 27 |
| Radio NSS / RESET | 18 / 23 |
| Radio DIO0 / DIO1 | 26 / 33 |
| DS18B20 | 13 (microSD inutilisée) |

La bande RX par défaut est **250 kHz** sur SX1276 ; les paramètres du portail proposent les bandes propres à cette puce. Le journal contient au maximum **120 lignes**, pour limiter la RAM statique de l'ESP32 classique.

La FIFO FSK du SX1276 ne contient que **64 octets**. La réception utilise donc le format variable : un octet de longueur et jusqu'à **63 octets de données**, avec validation matérielle du CRC CCITT (init `0x1D0F`, polynôme `0x1021`, complémenté). Les trames CRC invalides sont journalisées et rejetées. Le CRC reçu est retiré par la puce : la capture brute de 68 octets et la réparation logicielle du SX1262 ne sont pas disponibles sur cette variante. Voir la [documentation Semtech, sections FIFO et CRC](https://cdn.sparkfun.com/assets/7/7/3/2/2/SX1276_Datasheet.pdf).

Compilation et tests du traitement FIFO validés ; association et échanges radio à confirmer sur une carte réelle. Les configurations Heltec conservent la réception brute et la réparation CRC.

---

### 3️⃣ Configuration via le portail web

1. Se connecter au WiFi créé : `OpenFrisquetVisio-Setup`, mot de passe `frisquetconfig`
2. Ouvrir un navigateur sur `192.168.4.1`
3. Saisir le mot de passe du portail : `admin` par défaut, sans identifiant
4. Renseigner :
   - Vos **informations WiFi**
   - Vos **informations MQTT**
   - Vos **Les modules à utiliser**
   - **Mode passif Connect** si une box Connect est déjà présente
5. Sauvegarder → le module redémarre automatiquement

Le mot de passe du portail se modifie dans **Paramètres → Système → Accès au portail**.
Renseigner le nouveau mot de passe et sa confirmation, puis enregistrer.
Laisser les deux champs vides conserve le mot de passe actuel. Il est sauvegardé
sur l’appareil et conservé après redémarrage. Les pages et les API du portail
nécessitent une connexion ; les sessions expirent après 12 heures ou au
redémarrage, et un changement de mot de passe déconnecte toutes les sessions.
Le bouton **Se déconnecter** se trouve dans la même section.

---

### 4️⃣ Association avec la chaudière

#### 🔹 Module Connect
1. Sur la chaudière : **lancer l’association Connect**  
  a. Accédez au menu de configuration
  b. aller dans "partenaire" puis sur "Ajouter"
  c. Lancez l'association Frisquet Connect. Appuyez sur OK jusqu’à ce que l’écran demande d’associer la Frisquet Connect.
2. Sur le portail ou Home Assistant : activer le bouton **“Associer Connect”**
3. Une fois reconnu, la chaudière commencera à envoyer les données vers le module

#### 🔹 Mode passif Connect (box Connect déjà en place)
1. Activer **“Mode passif Connect”** dans le portail.
2. L’ESP32 **n’émet pas** de trames et **n’associe pas**.
3. Il récupère les informations en **écoutant les réponses chaudière** aux requêtes de la box Connect.

#### 🔹 Sonde extérieure
1. Sur la chaudière : **lancer l’association Sonde Extérieure**
2. Sur le portail ou Home Assistant : activer le bouton **“Associer Sonde Extérieure”**

Si une **DS18B20** est branchée, la température sera lue localement.
Sinon, envoyez la température via MQTT (ex. depuis un capteur météo HA), soit en modifiant l'entité sur HA, soit en publiant sur le topic **“"frisquet/sondeExterieure/temperatureExterieure/set"”**. (si Base Topic est toujours par défault à "frisquet" )

#### 🔹 Récupération du NetworkID (sans association)
1. Sur la chaudière, lancez une **association** :
   - soit une association Connect (ou remplacement)
   - soit **Remplacer Satellite Z1**
2. Sur le portail, cliquez sur **“Récupérer”** à côté du champ NetworkID.
3. Une fois le NetworkID affiché, **annulez l’action** côté chaudière.

---

## 🔄 Fonctionnement général

1. Le module se connecte au **WiFi** et au **broker MQTT**
2. Il **écoute** les trames radio LoRa de la chaudière (mode SX1262)
3. Il **publie** les mesures et états via MQTT
4. Home Assistant les découvre automatiquement via **MQTT Discovery**
5. Les commandes (modes, consignes, associations) envoyées depuis HA  
   sont traduites en trames radio vers la chaudière
6. Les infos satellites hors Connect (consigne boost) envoyées depuis HA  
   sont traduites en trames radio vers la chaudière par écrasement (non visible sur Satellites originaux)

---

## 🔧 Dépannage

| Problème | Cause possible | Solution |
|-----------|----------------|----------|
| Pas de données reçues sur Connect | Changer de consigne sur chacun des satellites de zone pour initialiser le Connect une première fois.

---

## 🧑‍💻 Crédits

Projet développé pour émuler le **Frisquet Connect** avec compatibilité Home Assistant.  
- Firmware basé sur **Arduino / PlatformIO**
- Utilise **RadioLib** (SX1262)
- Intégration **MQTT + Auto-discovery HA**

---

## ☕ Licence

Projet open-source à but expérimental.  
Utilisation à vos risques et périls — aucune affiliation avec Frisquet.  
Mais bon, si ça marche, vous pouvez toujours m’offrir un café ☕ 😉
[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/freedomnx)

---

**Auteur :** FreedomNX  
**Année :** 2025  
**Plateforme :** ESP32 (Heltec WiFi LoRa 32, SX1262)  
**Compatibilité :** Home Assistant, MQTT, Frisquet Chaudière série Eco Radio Visio  
**Version :** 26.10.24.1813
