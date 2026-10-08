# Compteur d'eau Zigbee — ESP32-H2

Compte les impulsions d'un compteur (contact sec, capteur Hall…), persiste le
total en NVS et le publie en Zigbee (cluster Metering) vers Zigbee2MQTT / Home
Assistant.

- ESP-IDF 5.5.x, cible `esp32h2`
- `espressif/esp-zigbee-lib` 2.0.4, **API native `ezbee`** (pas l'API compat 1.x `esp_zb_*`)
- Rôle Zigbee : **end device** non-sleepy (jamais routeur)

## Compilation

```bash
idf.py set-target esp32h2
idf.py menuconfig        # menu "Water counter" : GPIO, persistance, identité...
idf.py build flash monitor
```

## Câblage

| Fonction | GPIO (DevKitM-1) | Branchement |
|---|---|---|
| Entrée impulsions | GPIO10 | contact du compteur entre GPIO10 et **GND** |
| Bouton d'appairage | GPIO9 | bouton BOOT de la carte |
| LED d'état | GPIO8 | LED RGB WS2812 de la carte |

L'entrée impulsions utilise le **pull-up interne** : au repos le GPIO est à
l'état haut, la fermeture du contact le tire à GND, et l'impulsion est comptée
sur le **front descendant**, après l'anti-rebond. Un contact vers GND est plus
robuste qu'un contact vers 3V3 : un câble arraché ou en court-circuit avec la
masse ne peut pas mettre le 3V3 en défaut.

Le pull-up interne est faible (quelques dizaines de kΩ). Avec un câble long ou
dans un environnement électriquement bruité, ajouter un **pull-up externe** de
4,7 à 10 kΩ entre GPIO10 et 3V3. Le pull-up interne peut rester activé.

Pour tester sans compteur, relier brièvement GPIO10 à GND : chaque contact franc
d'au moins 30 ms, suivi d'un relâchement, compte une impulsion.

Autres câblages possibles dans `menuconfig` (*Water counter → Matériel*) :
contact vers 3V3 (pull-down, « niveau haut » activé), ou sortie active
push-pull (aucun tirage).

## Comportement

| Situation | LED |
|---|---|
| Démarrage de la stack | éteinte |
| Pas appairé | rouge fixe |
| Appairage en cours | orange clignotant |
| Appairé et fonctionnel | vert fixe |
| Appairé mais réseau injoignable (la stack retente seule) | rouge fixe |
| Impulsion comptée | flash bleu de 80 ms, puis retour à l'état précédent |

**Bouton (BOOT, GPIO9) maintenu 2 s** : lance l'appairage, quel que soit l'état.
Si le device est déjà sur un réseau, il le quitte d'abord proprement (les
datasets Zigbee sont effacés, le compteur est conservé). La recherche de réseau
est relancée toutes les 5 s pendant 3 minutes au maximum, puis la LED repasse
au rouge. Au premier démarrage, le device n'appaire **pas** tout seul : il attend
l'appui long.

Si la sortie du réseau n'aboutit pas en 5 s, le firmware fait un reset Zigbee
complet (redémarrage), puis reprend l'appairage automatiquement.

## Architecture

```
main.c          app_main() -> app_start()
app.c           logique applicative : file d'évènements unique et sa boucle,
                comptage, persistance, appairage, LED
pulse_input.c   comptage des impulsions (ISR + tâche d'anti-rebond dédiée)
button.c        bouton et appui long (ISR + petite tâche)
led.c           LED RGB : motif de fond + flash superposé
storage.c       NVS (namespace "watercount") + shadow du compteur en mémoire RTC
power_fail.c    détection optionnelle de coupure d'alimentation
zb_device.c     stack Zigbee : modèle ZCL, signaux, écritures d'attributs
z2m/            convertisseur externe Zigbee2MQTT
```

Les modules ne se connaissent pas entre eux. Chacun signale ses évènements par
un callback fourni par `app.c`, qui se contente de poster dans **une seule file**
traitée par **une seule boucle** (`app_task`). Toutes les décisions sont prises
là. Seules exceptions : l'anti-rebond des impulsions et du bouton, qui
tournent dans leurs propres tâches parce qu'ils dépendent du timing.

Les impulsions ne transitent pas une à une par la file : `pulse_input`
les accumule dans un compteur atomique que la boucle vide. Une file pleine ne
fait donc perdre aucune impulsion.

## Persistance du compteur

Le compteur est écrit en NVS :

- toutes les **10 impulsions** ;
- après **10 min sans nouvelle impulsion**, s'il reste des impulsions non écrites ;
- **avant un redémarrage logiciel** (`esp_restart()`, reset Zigbee, futur OTA) via
  un *shutdown handler* ;
- **immédiatement** lorsqu'on fixe sa valeur depuis Zigbee.

En plus, chaque impulsion est recopiée dans une **shadow en mémoire RTC non
initialisée**, protégée par un mot magique et une somme de contrôle. Elle ne
coûte rien en usure flash et survit aux resets que le code ne voit pas venir
(panic, watchdog, brownout avec retour de la tension). Au boot, si la shadow est
valide, c'est elle qui fait foi. **Aucune impulsion n'est donc perdue lors de ces resets.**

**Bouton RST et coupure d'alimentation franche** : le bouton RST de la devkit
(broche CHIP_PU) produit un reset complet de la puce (`reset reason 1`,
`ESP_RST_POWERON`), comme une coupure d'alimentation. La mémoire RTC est alors
perdue, et rien ne peut
être écrit au dernier moment sans aide matérielle. Le détecteur de brownout de
l'ESP32-H2 coupe l'alimentation de la flash avant d'appeler son handler (en
ISR), donc écrire en NVS à ce moment est impossible. Sans matériel dédié, on
perd au pire 9 impulsions ou 10 min d'impulsions. Pour atteindre zéro perte :
condensateur de réserve, plus surveillance de l'alimentation amont (5 V via un
pont diviseur) sur un GPIO déclaré dans `WC_POWER_FAIL_GPIO`. Le front
déclenche une écriture NVS immédiate (quelques ms).

Usure : une écriture du compteur occupe une entrée NVS de 32 octets, et NVS
n'écrit rien si la valeur n'a pas changé. La partition de 24 Kio (6 pages de
126 entrées, dont une page de réserve) est partagée avec les datasets Zigbee,
qui écrivent peu une fois le device appairé. À 50 écritures par jour, chaque
page est effacée environ toutes les deux semaines : 100 000 cycles représentent
plusieurs millénaires.

## Modèle Zigbee

Endpoint 1, profil HA, device ID `0x0053` (Meter Interface), clusters serveur :

- **Basic** : identité (voir plus bas), PowerSource = DC ;
- **Identify** : requis par le profil HA ;
- **Metering** (`0x0702`), attributs standard **en lecture seule** :
  - `CurrentSummationDelivered` = nombre d'impulsions, **reportable** (déclaré
    explicitement par le firmware, sans dépendre de l'accès par défaut de la lib) ;
  - `UnitOfMeasure` = litres (`0x07`) ou m³ (`0x01`) ;
  - `Multiplier` / `Divisor` tels que **volume = impulsions × Multiplier / Divisor** ;
  - `SummationFormatting`, `MeteringDeviceType` = eau.

La configuration passe par des attributs **manufacturer-specific** du cluster
Metering (code fabricant `0x131B`). Un client qui ne fournit pas ce code ne les
voit pas : c'est ce qui tient cette API à l'écart d'un usage normal.

| ID | Type | Rôle |
|---|---|---|
| `0xF000` | enum8 | unité : 0 = litres, 1 = m³ |
| `0xF001` | uint32 | volume d'une impulsion, en **µL** (1 L/imp. = 1 000 000) |
| `0xF002` | uint48 | compteur d'impulsions ; l'écrire recale le compteur |

Le volume par impulsion est exprimé en litres quelle que soit l'unité choisie.
Changer d'unité ne change donc pas la quantité physique comptée : seuls
`Multiplier`/`Divisor` sont recalculés. Ils sont limités à 24 bits ; si la
fraction exacte ne tient pas, le firmware prend la meilleure approximation
(fraction continue).

Le compteur est en impulsions, pas en volume : c'est la seule grandeur exacte
que le device connaît. La conversion en volume se fait côté contrôleur, avec
`Multiplier`/`Divisor`.

### Zigbee2MQTT / Home Assistant

Le convertisseur est **indispensable** : sans lui, Zigbee2MQTT génère une
définition automatique, et pour tout cluster `seMetering` il applique son modèle
de compteur électrique (`power`, `energy`), sans lire `MeteringDeviceType`.

Installation de `z2m/watercounter.js` (format CommonJS, compatible 1.x et 2.x) :

- **Zigbee2MQTT 2.x** : le copier dans le dossier `external_converters/` (à côté
  de `configuration.yaml`) ;
- **Zigbee2MQTT 1.x** : le copier à côté de `configuration.yaml` et le déclarer :
  ```yaml
  external_converters:
    - watercounter.js
  ```

Redémarrer Zigbee2MQTT : le log doit afficher
`Loaded external converter 'watercounter.js'`. Sur la page du device, le champ
« Support » ne doit plus indiquer `generated`. Si le device était déjà appairé,
lancer *Reconfigure* pour exécuter le `configure` (binding et reporting).

Le convertisseur expose :

- `water_consumed` (L ou m³) : Home Assistant le classe en `device_class: water`,
  `state_class: total_increasing`, donc il est utilisable dans le tableau de
  bord Énergie ;
- `counter` : nombre d'impulsions, à jour à chaque report et modifiable pour
  recaler le compteur sur l'index physique ;
- `unit`, `litres_per_pulse` et `counter` sont en catégorie **configuration**, donc
  rangés dans la section « Configuration » de la fiche de l'appareil dans HA,
  et non dans les contrôles principaux.

Le `configure` demande un reporting de `currentSummDelivered` toutes les 5 s au
plus, toutes les heures au moins, avec un seuil de variation de **0** : la stack
n'envoie un report que si la variation est strictement supérieure au seuil, et
un seuil de 1 ne faisait remonter qu'une impulsion sur deux.

Après un changement d'unité, le convertisseur demande à Zigbee2MQTT de
recalculer les entités : l'unité de `water_consumed` suit, et la découverte Home
Assistant est republiée. Home Assistant signalera alors un changement d'unité
dans les statistiques long terme de l'entité, à accepter une fois.

## Identité du device : où elle sert

Toutes ces valeurs sont dans `menuconfig` > *Water counter* > *Identité Zigbee*,
sauf mention contraire.

| Donnée | Où elle va | Qui l'utilise |
|---|---|---|
| `ManufacturerName` | Basic `0x0004` | **Zigbee2MQTT, pour choisir le convertisseur** (avec le modèle) ; affiché comme fabricant |
| `ModelIdentifier` | Basic `0x0005` | **clé de correspondance** avec `zigbeeModel` du convertisseur ; ne plus la changer une fois des devices déployés |
| `DateCode` | Basic `0x0006`, calculé à la compilation (AAAAMMJJ) | affiché par Z2M (date du firmware) |
| `SWBuildID` | Basic `0x4000`, version de l'app (`git describe` ou `PROJECT_VER`) | affiché par Z2M et HA comme version du firmware |
| `ApplicationVersion`, `HWVersion` | Basic `0x0001`, `0x0003` | informatif |
| `SerialNumber` | Basic `0x000D`, calculé : EUI-64 de la puce | informatif ; unique sans rien configurer |
| Code fabricant | node descriptor + attributs de config | ne s'affiche nulle part ; doit être identique dans le firmware et le convertisseur ; servira aussi aux images OTA |
| Device ID `0x0053` | simple descriptor (`zb_device.c`) | ZHA et les contrôleurs génériques ; Z2M l'ignore |
| Adresse IEEE | puce | identifiant réseau ; nom par défaut dans Z2M |

Le nom, la description et l'icône affichés dans Home Assistant viennent du
convertisseur (`vendor`, `description`), pas du device.

Suggestions :

- `ModelIdentifier` : court, ASCII, sans espace, stable : `WaterCounter-H2` ou
  une référence maison du type `KB-WMC1`. N'en changer que si le modèle ZCL
  devient incompatible, par exemple `KB-WMC2`.
- `ManufacturerName` : un nom de « marque » DIY stable (`DIY` est une valeur
  provisoire).
  Éviter de se faire passer pour un vrai fabricant : Z2M pourrait choisir son
  convertisseur.
- Code fabricant : garder `0x131B` (Espressif), l'usage courant pour un device
  DIY sur ESP32. Il n'existe pas de plage « privée » officielle en Zigbee.
- Versions : incrémenter `ApplicationVersion` à chaque release ; elle et
  `SWBuildID` serviront de base au futur OTA.

## Seuil LQI d'appairage

esp-zigbee-lib 2.0.4 ignore, à l'appairage comme au rejoin, les parents dont le
LQI est inférieur à 75. Or l'ESP32-H2 rapporte des LQI bas : 63 mesuré pour un
RSSI de -65 dBm (vraisemblablement LQI = RSSI + 128). Avec ce seuil, des parents
corrects étaient rejetés et le device échouait par intermittence à s'appairer
ou à se reconnecter après un reset (`status 3`, `NO_NETWORK`). Le firmware
abaisse le seuil à 40 (environ -88 dBm), réglable dans `menuconfig` :
*Water counter → Appairage Zigbee → LQI minimal d'un parent*.

Le LQI et le RSSI des voisins sont journalisés en niveau debug à chaque
connexion (`CONFIG_LOG_MAXIMUM_LEVEL` à Debug dans `menuconfig`, puis
`esp_log_level_set("zb_device", ESP_LOG_DEBUG)`). Un RSSI de 127 signifie « pas
encore mesuré ».

## Stockage

Une seule partition `nvs`, comme dans les exemples officiels esp-zigbee-sdk. La
stack y range ses datasets dans le namespace `zigbee` (elle ne l'initialise pas,
d'où le `nvs_flash_init()` fait par l'application avant de démarrer Zigbee).
Son reset n'efface que ce namespace (`nvs_erase_all`), jamais le compteur rangé
dans `watercount`. La partition FAT `zb_fct` des exemples officiels n'est
référencée nulle part dans la lib 2.0.4 : elle est omise.

## Énergie et passage éventuel sur pile

Choix déjà faits pour ne pas compromettre un passage sur pile :

- end device, jamais routeur ;
- comptage par interruption GPIO (aucun polling), sur GPIO10, qui fait partie
  des GPIO LP (7 à 14) capables de réveiller la puce d'un deep sleep ;
- le node descriptor déclare l'alimentation, à passer en batterie le moment venu.

Aujourd'hui (secteur), la radio reste allumée (`rx_on_when_idle`) pour que le
device réponde immédiatement, et le light sleep n'est pas activé. Pour une
version sur pile, il faudra :

- `rx_on_when_idle = false` + `CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`,
  `CONFIG_IEEE802154_SLEEP_ENABLE` (voir l'exemple officiel `light_sleep_end_device`) ;
- `gpio_wakeup_enable()` sur l'entrée impulsions ;
- un cluster Power Configuration (niveau de pile) ;
- une LED éteinte par défaut et un keep-alive allongé ;
- un pull-up consomme en continu tant que le contact reste fermé (3,3 V / R :
  environ 0,3 mA avec 10 kΩ), ce qui arrive si le compteur s'arrête contact
  fermé ; préférer alors une résistance plus forte ;
- sur l'ESP32-H2, les GPIO 7 à 14 utilisés comme sources de réveil (fonction LP)
  n'ont plus de pull-up ni de pull-down pilotables (`soc_caps.h` de l'IDF) : une
  version en deep sleep aura besoin d'un **pull-up externe**.

## État de validation sur le matériel

Compilé sans warning avec ESP-IDF 5.5.4 (la 5.5.5 existe mais n'est pas
installée ici).

Validé sur la carte, avec Zigbee2MQTT 2.x :

- appairage, et rejoin immédiat après un reset (une fois le seuil LQI corrigé) ;
- convertisseur chargé : `water_consumed`, `unit` et `litres_per_pulse` sont
  publiés, donc les attributs manufacturer-specific sont lisibles ; l'écriture de
  `unit` est prise en compte par le firmware et l'unité suit dans Z2M ;
- comptage de bout en bout : chaque impulsion remonte dans Z2M (seuil de
  reporting à 0, cf. plus haut) ;
- repli par reset Zigbee complet quand le device ne peut pas quitter son réseau
  (appui long alors que le réseau est injoignable).

Reste à valider :

1. qu'une écriture Zigbee sur les attributs manufacturer-specific déclenche bien
   `EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID` (journaux `app: Unit set to…`,
   `app: Counter set to…`) ;
2. qu'un appui long sur un device connecté le fait sortir proprement du réseau
   (signal LEAVE émis par `ezb_bdb_reset_via_local_action()`).
