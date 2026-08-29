"use strict";

var fs = require("fs");
var vm = require("vm");
var values = {};
var parameters = {};
var sysex = [];
var calls = [];

function k(group, key) { return group + "|" + key; }

global.engine = {
    getValue: function(group, key) {
        if (values[k(group, key)] !== undefined) return values[k(group, key)];
        if (key === "duration") return 420;
        if (key === "bpm") return 128;
        if (key === "rate_ratio") return 1;
        if (key === "beat_prev") return 100;
        if (key === "beat_next") return 200;
        if (key === "track_samplerate") return 44100;
        if (key.indexOf("hotcue_") === 0) return -1;
        if (key.indexOf("browse_track_id_") === 0) return 1;
        return 0;
    },
    setValue: function(group, key, value) {
        values[k(group, key)] = value;
        calls.push(["value", group, key, value]);
    },
    getParameter: function(group, key) {
        return parameters[k(group, key)] || 0;
    },
    setParameter: function(group, key, value) {
        parameters[k(group, key)] = value;
        calls.push(["parameter", group, key, value]);
    },
    makeConnection: function() { return {disconnect: function() {}}; },
    beginTimer: function() { return 1; },
    stopTimer: function() {}
};
global.midi = {
    sendSysexMsg: function(message, length) {
        if (message.length !== length || message[0] !== 0xF0 || message[length - 1] !== 0xF7)
            throw new Error("invalid SysEx");
        sysex.push(message);
    }
};

vm.runInThisContext(fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2-scripts.js", "utf8"));

D2.init("test", false);
var groups = ["[Channel1]", "[Channel2]"];
groups.forEach(function(group) {
    D2.browseTouch(0, 0x64, 0x7F, 0, group);
    var beforeBrowse = calls.length;
    D2.browseEncoder(0, 0x14, 0x41, 0, group);
    if (!calls.slice(beforeBrowse).some(function(call) {
        return call[0] === "value" && call[2] === "d2_track_down" && call[3] === 1;
    })) throw new Error("clockwise browse did not pulse direct D2 track down");
    beforeBrowse = calls.length;
    D2.shiftButton(0, 0x5A, 0x7F, 0, group);
    D2.browseEncoder(0, 0x14, 0x3F, 0, group);
    if (!calls.slice(beforeBrowse).some(function(call) {
        return call[0] === "value" && call[2] === "d2_track_up" && call[3] === 1;
    })) throw new Error("counter-clockwise browse did not pulse direct D2 track up");
    D2.shiftButton(0, 0x5A, 0, 0, group);
    D2.backButton(0, 0x3F, 0x7F, 0, group);
    var beforeTreePress = calls.length;
    D2.loadSelectedTrack(0, 0x3E, 0x7F, 0, group);
    var treeCalls = calls.slice(beforeTreePress);
    if (!treeCalls.some(function(call) {
        return call[0] === "value" && call[1] === "[Library]" &&
            call[2] === "d2_sidebar_activate" && call[3] === 1;
    })) throw new Error("sidebar press did not activate direct D2 sidebar control");
    if (treeCalls.some(function(call) { return call[2] === "LoadSelectedTrack"; }))
        throw new Error("sidebar press unexpectedly loaded a track");
    D2.libraryFocus = 0;
    values[k("[Library]", "focused_widget")] = 3;
    var beforeTrackPress = calls.length;
    D2.loadSelectedTrack(0, 0x3E, 0x7F, 0, group);
    if (!calls.slice(beforeTrackPress).some(function(call) {
        return call[0] === "value" && call[2] === "LoadSelectedTrack" && call[3] === 1;
    })) throw new Error("track-list press did not load selected track");
    for (var note = 0x31; note <= 0x34; note++)
        D2.leftScreenButton(0, note, 0x7F, 0, group);
    for (note = 0x35; note <= 0x38; note++)
        D2.rightScreenButton(0, note, 0x7F, 0, group);
    ["HOTCUE", "LOOP", "FREEZE", "SAMPLER", "BEATJUMP"].forEach(function(mode) {
        D2.setPerformanceMode(group, mode);
        for (var pad = 0x4C; pad <= 0x53; pad++) D2.padButton(0, pad, 0x7F, 0, group);
    });
    for (var cc = 0x24; cc <= 0x27; cc++) D2.fxKnob(0, cc, 64, 0, group);
    for (note = 0x28; note <= 0x2B; note++) D2.fxButton(0, note, 0x7F, 0, group);
    for (note = 0x2C; note <= 0x2F; note++) {
        D2.fxTouch(0, note, 0x7F, 0, group);
        D2.fxTouch(0, note, 0, 0, group);
    }
    D2.touchStripTouch(0, 0x5E, 0x7F, 0, group);
    D2.touchStrip(0, 0x28, 64, 0, group);
    D2.touchStripTouch(0, 0x5E, 0, 0, group);
    D2.captureButton(0, 0x40, 0x7F, 0, group);
    D2.editButton(0, 0x41, 0x7F, 0, group);
    D2.screenEncoder(0, 0x10, 0x41, 0, group);
    D2.screenEncoder(0, 0x11, 0x3F, 0, group);
});
D2.shutdown();

if (sysex.length < 20 || calls.length < 100) throw new Error("coverage failure");
function decodeSysex(message) {
    return message.slice(2, message.length - 1).map(function(byte) {
        return String.fromCharCode(byte);
    }).join("");
}
var sysexText = sysex.map(decodeSysex);
if (!sysexText.some(function(text) { return text.indexOf("|LOOPSIZE|4.00000") >= 0; }))
    throw new Error("loop size was not published to the D2 renderer");
if (!sysexText.some(function(text) { return text.indexOf("|BROWSE8|") >= 0; }))
    throw new Error("ninth Browse row was not published");
console.log("D2_MAPPING_TEST_OK sysex=" + sysex.length + " calls=" + calls.length);
