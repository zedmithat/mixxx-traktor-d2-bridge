/* Smoke-test the controller in a Mixxx-like VM without touching hardware. */
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const values = new Map();
const calls = [];
const connections = [];
const timers = [];
const midiMessages = [];

function key(group, control) { return group + "\u0000" + control; }
function get(group, control) {
    return values.has(key(group, control)) ? values.get(key(group, control)) : 0;
}
function set(group, control, value) {
    values.set(key(group, control), value);
    calls.push({ op: "set", group, control, value });
}

const engine = {
    getValue: get,
    setValue: set,
    setParameter: set,
    makeConnection(group, control, callback) {
        const connection = { group, control, callback,
            disconnect() { this.disconnected = true; } };
        connections.push(connection);
        return connection;
    },
    beginTimer(interval, callback) {
        timers.push({ interval, callback });
        return timers[timers.length - 1];
    },
    stopTimer(timer) { timer.stopped = true; }
};
const midi = {
    sendShortMsg(status, control, velocity) {
        midiMessages.push({ status, control, velocity });
    }
};

const source = fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2-comprehensive.js", "utf8");
const context = { engine, midi, console, isFinite, Math, Number, String };
vm.runInNewContext(source, context, { filename: "Traktor-Kontrol-D2-comprehensive.js" });
const d2 = context.D2Comprehensive;
assert.ok(d2, "controller object is exported to the Mixxx script global");
assert.equal(typeof context.init, "function");
assert.equal(typeof context.shutdown, "function");
assert.equal(typeof context.refreshLEDs, "function");

/* Initialisation connects every requested LED source and starts blinking. */
d2.init();
assert.equal(d2.currentDeck, 1);
assert.equal(timers.length, 1);
assert.ok(connections.length >= 4 * (7 + 8) + 4 + 4 * 3);

/* Deck selection and transport use the selected channel dynamically. */
d2.selectDeck(2);
d2.play(0, 0, 127, 0, "[Channel1]");
d2.sync(0, 0, 127, 0, "[Channel1]");
d2.flux(0, 0, 127, 0, "[Channel1]");
assert.ok(calls.some(c => c.group === "[Channel2]" && c.control === "play"));
assert.ok(calls.some(c => c.group === "[Channel2]" && c.control === "beatsync"));
assert.ok(calls.some(c => c.group === "[Channel2]" && c.control === "slip_enabled"));

/* Relative browse values must move in both directions, including shift pages. */
d2.browse(0, 0, 1, 0, "[Channel2]");
d2.browse(0, 0, 0x3F, 0, "[Channel2]");
d2.shift(0, 0, 1);
d2.browse(0, 0, 1, 0, "[Channel2]");
assert.ok(calls.some(c => c.group === "[Library]" && c.control === "MoveDown"));
assert.ok(calls.some(c => c.group === "[Library]" && c.control === "MoveUp"));
assert.equal(calls.filter(c => c.group === "[Library]" && c.control === "MoveDown").length, 18);
d2.shift(0, 0, 0);

/* Pads, FX, samplers, load/back/view, and touch strip all dispatch. */
d2.pad(0, d2.LED.pads, 127, 0, "[Channel2]");
d2.shift(0, 0, 1);
d2.pad(0, d2.LED.pads, 127, 0, "[Channel2]");
d2.shift(0, 0, 0);
assert.ok(calls.some(c => c.group === "[Channel2]" && c.control === "hotcue_1_activate"));
assert.ok(calls.some(c => c.group === "[Channel2]" && c.control === "hotcue_1_clear"));
d2.setPadMode("LOOP");
d2.pad(0, d2.LED.pads + 4, 127, 0, "[Channel2]");
d2.setPadMode("REMIX");
d2.pad(0, d2.LED.pads + 1, 127, 0, "[Channel2]");
assert.ok(calls.some(c => c.control === "beatloop_5_toggle"));
assert.ok(calls.some(c => c.group === "[Sampler2]" && c.control === "start_play"));
d2.fxKnob(0, 0x24, 64, 0, "[Channel2]");
d2.fxKnob(0, 0x25, 64, 0, "[Channel2]");
d2.fxKnob(0, 0x26, 64, 0, "[Channel2]");
d2.fxKnob(0, 0x27, 64, 0, "[Channel2]");
assert.ok(calls.some(c => c.control === "parameter1"));
assert.ok(calls.some(c => c.control === "parameter2"));
assert.ok(calls.some(c => c.control === "parameter3"));
assert.ok(calls.some(c => c.control === "mix"));
d2.load(0, 0, 127, 0, "[Channel2]");
d2.back(0, 0, 127);
d2.view(0, 0, 127);
d2.touchStrip(0, 0, 64, 0, "[Channel2]");
assert.ok(calls.some(c => c.control === "LoadSelectedTrack"));
assert.ok(calls.some(c => c.control === "GoToSidebar"));
assert.ok(calls.some(c => c.control === "toggle_main_elements"));
assert.ok(calls.some(c => c.control === "jog"));

/* LED refresh and shutdown must emit messages and stop their timer. */
d2.blinkTick();
assert.ok(midiMessages.length > 0);
d2.shutdown();
assert.equal(timers[0].stopped, true);
assert.ok(midiMessages.some(m => m.velocity === 0));

console.log("D2_COMPREHENSIVE_TEST_OK calls=" + calls.length +
            " connections=" + connections.length +
            " midi=" + midiMessages.length);
