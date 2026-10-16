/*
 * Comprueba la coherencia del i18n del panel (src/web/index.html).
 *
 *  - Extrae las claves referenciadas (data-i18n* del HTML y T('...') del JS).
 *  - Comprueba que existan en TODOS los idiomas de I18N.
 *  - Avisa de claves declaradas en unos idiomas y no en otros.
 *
 * Uso:  node tools/check-i18n.js
 * Salida: 0 si todo cuadra, 1 si hay problemas.
 */
'use strict';

const fs = require('fs');
const path = require('path');

const file = path.join(__dirname, '..', 'src', 'web', 'index.html');
const html = fs.readFileSync(file, 'utf8');

const scriptMatch = html.match(/<script>([\s\S]*)<\/script>/);
if (!scriptMatch) {
    console.error('No se encontró el <script> en ' + file);
    process.exit(1);
}
const script = scriptMatch[1];

const start = script.indexOf('var I18N=');
if (start < 0) {
    console.error('No se encontró el objeto I18N');
    process.exit(1);
}
/* Delimita el literal contando llaves, para no depender del formato. */
const open = script.indexOf('{', start);
let depth = 0;
let close = -1;
for (let i = open; i < script.length; i++) {
    if (script[i] === '{') depth++;
    else if (script[i] === '}') {
        depth--;
        if (depth === 0) { close = i + 1; break; }
    }
}
if (close < 0) {
    console.error('No se pudo delimitar el objeto I18N');
    process.exit(1);
}
const dictSource = script.slice(open, close);

let I18N;
try {
    I18N = eval('(' + dictSource + ')');
} catch (e) {
    console.error('I18N no es un literal válido: ' + e.message);
    process.exit(1);
}

const languages = Object.keys(I18N);
if (!languages.length) {
    console.error('I18N está vacío');
    process.exit(1);
}

const keys = new Set();
for (const m of html.matchAll(/data-i18n(?:-html|-placeholder|-aria)?="([^"]+)"/g)) keys.add(m[1]);
for (const m of script.matchAll(/\bT\('([^']+)'/g)) keys.add(m[1]);
for (const m of script.matchAll(/\bT\("([^"]+)"/g)) keys.add(m[1]);

/* Prefijos compuestos en tiempo de ejecución (p. ej. T('error.' + codigo)). */
const prefixes = ['error.', 'state.'];

/* Claves compuestas en tiempo de ejecución (código + estado del torrent). */
const dynamic = ['error.post_required','error.magnet_required','error.magnet_invalid',
    'error.magnet_no_metadata','error.path_invalid','error.action_invalid',
    'error.file_required','error.empty_file','error.no_file','error.multipart_required',
    'error.invalid_torrent','error.start_failed','error.no_tracker','error.no_peers',
    'error.peers_failed','error.write_failed','error.tracker_failed','error.parse_failed',
    'error.hash_mismatch','state.downloading','state.done','state.seeding',
    'state.stopped','state.error'];
for (const k of dynamic) keys.add(k);

const sorted = [...keys].sort();
let problems = 0;

for (const k of sorted) {
    if (prefixes.includes(k)) continue;  /* prefijo dinámico, no es una clave */
    const missing = languages.filter((l) => !(k in I18N[l]));
    if (missing.length) {
        console.error('FALTA  ' + k + '  en: ' + missing.join(', '));
        problems++;
    }
}

for (const k of Object.keys(I18N[languages[0]])) {
    const missing = languages.filter((l) => !(k in I18N[l]));
    if (missing.length) {
        console.error('DECLARADA SOLO EN ALGUNOS IDIOMAS  ' + k + '  falta en: ' + missing.join(', '));
        problems++;
    }
}

const declared = new Set(Object.keys(I18N[languages[0]]));
const unused = [...declared].filter((k) => !keys.has(k));

console.log('Idiomas: ' + languages.join(', '));
console.log('Claves declaradas: ' + declared.size + ' · claves referenciadas: ' + sorted.length);
if (unused.length) console.log('Sin uso (informativo): ' + unused.join(', '));
if (problems) {
    console.error('\ni18n INCOMPLETO: ' + problems + ' problema(s)');
    process.exit(1);
}
console.log('i18n coherente.');
