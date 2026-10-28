# 🧠 OpenFrisquetVisio

## Candidat de préparation V4.3 — émission interdite

Ce candidat local n'est pas une mise en service. Toutes les émissions passent
par `Radio::transmit`, qui reste inhibé sans chemin d'activation : la relation
SX1262 → FEM KCT8103L → antenne/ERP n'est pas qualifiée. La consigne explicite
−9 dBm est celle de la puce, PAS une puissance à l'antenne. Aucun essai radio
ou thermique, aucune conformité RF mesurée n'est revendiquée.

Le garde global réserve avant chaque tentative (y compris association/retries)
au plus 3,6 s par heure glissante, via 61 tranches d'une minute. Il surcompte
le timeout RadioLib (5 ms + 5× temps nominal, arrondi conservateur) et impose
une heure sans TX après boot pour couvrir le budget antérieur inconnu.
Ce silence réglementaire peut empêcher un ordre de sécurité après reboot :
une garantie physique de coupure de chauffe n'est donc PAS livrée par ce build.

Contrôle virtuel, sans changer les topics scalaires existants :

1. Le producteur lit `<baseTopic>/controlSession`, défi renouvelé à chaque
   connexion MQTT. Le défi n'est ni une identité ni une autorisation.
2. Toutes les 60 s, il recalcule les données courantes et publie sans retain
   `z1/mode/set`, `z1/temperatureConsigne/set`, `z1/temperatureAmbiante/set`
   (ordre libre), puis `z1/controlHeartbeat/set` = `<défi>:<séquence uint64>`.
   La séquence doit croître strictement dans cette session. Chaque heartbeat
   exige les trois commandes valides reçues depuis le heartbeat précédent,
   chacune âgée d'au plus 60 s. Les profils seuls ne qualifient rien.
3. Le bail expire exactement à 1 800 000 ms depuis le dernier heartbeat accepté,
   indépendamment des mises à jour scalaires suivantes et de l'horloge civile.
   Boot/restauration NVS, perte MQTT/Wi-Fi, reconnexion et tentative de réassociation
   désarment le contrôle ; il faut de nouvelles données et un nouveau heartbeat.
   Les temps d'origine de réception ne sont jamais restaurés comme « maintenant ».
   Ce protocole atteste un producteur actif ; il ne mesure pas l'âge physique
   du capteur, que le producteur doit qualifier avant de publier.

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
