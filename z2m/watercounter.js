// Convertisseur externe Zigbee2MQTT pour le compteur d'eau ESP32-H2.
//
// Format CommonJS : chargé tel quel par Zigbee2MQTT 2.x (dossier
// external_converters/) comme par 1.x (clé external_converters de
// configuration.yaml). Redémarrer Zigbee2MQTT après l'avoir copié.
// Les constantes ci-dessous doivent correspondre au menuconfig du firmware
// (menu "Water counter" > "Identité Zigbee").
//
// Entités exposées :
//  - water_consumed   : volume total (L ou m³ selon la configuration), mappé par
//                       Home Assistant en device_class water / total_increasing ;
//  - counter          : nombre d'impulsions, à jour à chaque report et modifiable
//                       pour recaler le compteur (catégorie "config" dans HA) ;
//  - unit, litres_per_pulse : configuration (catégorie "config" dans HA).

const {Zcl} = require('zigbee-herdsman');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');

const MANUFACTURER_NAME = 'DIY';
const MODEL_IDENTIFIER = 'WaterCounter-H2';
const MANUFACTURER_CODE = 0x131b;
const ENDPOINT = 1;

const ATTR_UNIT = 0xf000;
const ATTR_PULSE_VOLUME = 0xf001;
const ATTR_COUNTER = 0xf002;
const UNIT_OF_MEASURE_LITRES = 0x07;
const UINT48_MAX = 0xffffffffffff;

const e = exposes.presets;
const ea = exposes.access;
const manufacturerOptions = {manufacturerCode: MANUFACTURER_CODE};

// zigbee-herdsman actuel renvoie un uint48 en number ; les anciennes versions
// le renvoyaient sous forme [poids fort, poids faible]. Number() couvre aussi BigInt.
const toNumber = (value) => (Array.isArray(value) ? value[0] * 0x100000000 + value[1] : Number(value));

const fzWaterCounter = {
    cluster: 'seMetering',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const device = msg.device;
        const previous = device.meta.watercounter ?? {};
        const format = {...previous};
        let formatChanged = false;
        for (const key of ['unitOfMeasure', 'multiplier', 'divisor']) {
            if (msg.data[key] !== undefined) {
                format[key] = Number(msg.data[key]);
                formatChanged = true;
            }
        }
        if (formatChanged) {
            device.meta.watercounter = format;
            device.save();
            // L'unité de water_consumed dépend de unitOfMeasure : on demande à Z2M de
            // recalculer les exposes, ce qui republie aussi la découverte Home Assistant.
            if (format.unitOfMeasure !== previous.unitOfMeasure) {
                meta.deviceExposesChanged();
            }
        }

        const result = {};
        if (msg.data.currentSummDelivered !== undefined) {
            // Le firmware tient l'attribut ATTR_COUNTER égal à CurrentSummationDelivered :
            // les reports de ce dernier suffisent à garder counter à jour.
            const pulses = toNumber(msg.data.currentSummDelivered);
            result.counter = pulses;
            if (format.multiplier && format.divisor) {
                result.water_consumed = (pulses * format.multiplier) / format.divisor;
            }
        }
        if (msg.data[ATTR_UNIT] !== undefined) {
            result.unit = msg.data[ATTR_UNIT] === 0 ? 'litres' : 'm3';
        }
        if (msg.data[ATTR_PULSE_VOLUME] !== undefined) {
            result.litres_per_pulse = msg.data[ATTR_PULSE_VOLUME] / 1e6;
        }
        if (msg.data[ATTR_COUNTER] !== undefined) {
            result.counter = toNumber(msg.data[ATTR_COUNTER]);
        }
        return result;
    },
};

const tzWaterCounter = {
    key: ['unit', 'litres_per_pulse', 'counter', 'water_consumed'],
    convertSet: async (entity, key, value, meta) => {
        switch (key) {
            case 'unit': {
                const unit = value === 'litres' ? 0 : 1;
                await entity.write('seMetering', {[ATTR_UNIT]: {value: unit, type: Zcl.DataType.ENUM8}}, manufacturerOptions);
                break;
            }
            case 'litres_per_pulse': {
                const microlitres = Math.round(Number(value) * 1e6);
                if (!(microlitres >= 1 && microlitres <= 0xffffffff)) {
                    throw new Error(`litres_per_pulse hors limites : ${value}`);
                }
                await entity.write(
                    'seMetering',
                    {[ATTR_PULSE_VOLUME]: {value: microlitres, type: Zcl.DataType.UINT32}},
                    manufacturerOptions,
                );
                break;
            }
            case 'counter': {
                const pulses = Math.round(Number(value));
                if (!(pulses >= 0 && pulses <= UINT48_MAX)) {
                    throw new Error(`counter hors limites : ${value}`);
                }
                await entity.write('seMetering', {[ATTR_COUNTER]: {value: pulses, type: Zcl.DataType.UINT48}}, manufacturerOptions);
                break;
            }
            default:
                throw new Error(`${key} n'est pas modifiable`);
        }
        // Relit la représentation standard pour recalculer water_consumed.
        await entity.read('seMetering', ['unitOfMeasure', 'multiplier', 'divisor', 'currentSummDelivered']);
        return {state: {[key]: value}};
    },
    convertGet: async (entity, key, meta) => {
        if (key === 'water_consumed') {
            await entity.read('seMetering', ['currentSummDelivered']);
        } else {
            await entity.read('seMetering', [ATTR_UNIT, ATTR_PULSE_VOLUME, ATTR_COUNTER], manufacturerOptions);
        }
    },
};

module.exports = {
    zigbeeModel: [MODEL_IDENTIFIER],
    model: MODEL_IDENTIFIER,
    vendor: MANUFACTURER_NAME,
    description: 'Compteur d\'eau à impulsions (ESP32-H2)',
    fromZigbee: [fzWaterCounter],
    toZigbee: [tzWaterCounter],
    // Fonction : l'unité de water_consumed suit la configuration du device ; elle est
    // recalculée dès que le device annonce une nouvelle unité (voir fzWaterCounter).
    exposes: (device, options) => {
        const litres = device?.meta?.watercounter?.unitOfMeasure === UNIT_OF_MEASURE_LITRES;
        return [
            e.numeric('water_consumed', ea.STATE_GET)
                .withUnit(litres ? 'L' : 'm³')
                .withDescription('Volume total compté'),
            e.enum('unit', ea.ALL, ['litres', 'm3']).withDescription('Unité du volume').withCategory('config'),
            e.numeric('litres_per_pulse', ea.ALL)
                .withUnit('L')
                .withValueMin(0.000001)
                .withValueStep(0.000001)
                .withDescription('Volume correspondant à une impulsion')
                .withCategory('config'),
            e.numeric('counter', ea.ALL)
                .withValueMin(0)
                .withValueMax(UINT48_MAX)
                .withDescription('Compteur d\'impulsions ; écrire une valeur pour le recaler sur le compteur physique')
                .withCategory('config'),
        ];
    },
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(ENDPOINT);
        await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering']);
        await endpoint.read('seMetering', ['unitOfMeasure', 'multiplier', 'divisor']);
        await endpoint.read('seMetering', [ATTR_UNIT, ATTR_PULSE_VOLUME, ATTR_COUNTER], manufacturerOptions);
        // change: 0 et non 1 : esp-zigbee-lib 2.0.4 n'envoie un report que si la variation
        // est strictement supérieure au seuil. Avec 1, seule une impulsion sur deux remontait.
        await reporting.currentSummDelivered(endpoint, {min: 5, max: 3600, change: 0});
        await endpoint.read('seMetering', ['currentSummDelivered']);
    },
};
