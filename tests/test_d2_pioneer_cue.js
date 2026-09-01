"use strict";

var fs = require("fs");
var vm = require("vm");
var state = {};
var requests = [];
var nativePreview = {};

function key(group, control) { return group + "|" + control; }
function get(group, control) { return state[key(group, control)] || 0; }
function put(group, control, value) { state[key(group, control)] = value; }
function assert(condition, message) { if (!condition) throw new Error(message); }

function nativeSet(group, control, value) {
    requests.push([group, control, value]);
    if (control === "cue_cdj") {
        put(group, control, value);
        var samples = get(group, "track_samples");
        var cueRatio = samples ? get(group, "cue_point") / samples : 0;
        if (value) {
            if (get(group, "play")) {
                put(group, "play", 0);
                put(group, "playposition", cueRatio);
                nativePreview[group] = false;
            } else if (Math.abs(get(group, "playposition") - cueRatio) < 0.000001) {
                put(group, "play", 1);
                nativePreview[group] = true;
            } else {
                put(group, "cue_point", get(group, "playposition") * samples);
                nativePreview[group] = false;
            }
        } else if (nativePreview[group]) {
            put(group, "play", 0);
            put(group, "playposition", cueRatio);
            nativePreview[group] = false;
        }
        return;
    }
    if (control === "play" && !value && nativePreview[group]) {
        nativePreview[group] = false;
        put(group, "play", 1);
        return;
    }
    put(group, control, value);
}

global.engine = {
    getValue: get,
    setValue: nativeSet,
    beginTimer: function() { return 1; },
    getParameter: function() { return 0; },
    setParameter: function() {},
    stopTimer: function() {}
};
global.midi = {sendSysexMsg: function() {}};

vm.runInThisContext(fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2-scripts.js", "utf8"));

function prepare(group) {
    put(group, "track_samples", 100000);
    put(group, "cue_point", 20000);
    put(group, "playposition", 0.20);
    put(group, "play", 0);
    put(group, "hotcue_1_position", 70000);
    put(group, "hotcue_1_enabled", 1);
    D2.cueHeld[group] = false;
    D2.cuePreviewing[group] = false;
}

function runSurface(surface, deck) {
    var group = "[Channel" + deck + "]";
    D2.activeDeck[surface] = deck;
    prepare(group);

    /* PLAY at cue -> normal playback; PLAY again -> pause in place. */
    D2.playButton(0, 0x71, 0x7F, 0, surface);
    assert(get(group, "play") === 1, surface + " PLAY did not start");
    put(group, "playposition", 0.35);
    D2.playButton(0, 0x71, 0x7F, 0, surface);
    assert(get(group, "play") === 0 && get(group, "playposition") === 0.35,
        surface + " PLAY did not pause in place");

    /* Paused away -> CUE sets a new Main Cue and does not preview. */
    D2.cueButton(0, 0x70, 0x7F, 0, surface);
    assert(get(group, "cue_point") === 35000 && get(group, "play") === 0,
        surface + " paused CUE did not set Main Cue only");
    D2.cueButton(0, 0x70, 0, 0, surface);

    /* At cue -> hold previews; release returns and pauses. */
    D2.cueButton(0, 0x70, 0x7F, 0, surface);
    assert(get(group, "play") === 1 && D2.cuePreviewing[surface],
        surface + " CUE hold did not start preview");
    put(group, "playposition", 0.42);
    D2.cueButton(0, 0x70, 0, 0, surface);
    assert(get(group, "play") === 0 && get(group, "playposition") === 0.35,
        surface + " CUE release did not return to Main Cue");

    /* CUE hold + PLAY latches; the later CUE release must not stop it. */
    D2.cueButton(0, 0x70, 0x7F, 0, surface);
    D2.playButton(0, 0x71, 0x7F, 0, surface);
    D2.cueButton(0, 0x70, 0, 0, surface);
    assert(get(group, "play") === 1 && !D2.cuePreviewing[surface],
        surface + " CUE+PLAY did not latch normal playback");

    /* Playing CUE -> return to Main Cue and pause. */
    put(group, "playposition", 0.60);
    D2.cueButton(0, 0x70, 0x7F, 0, surface);
    assert(get(group, "play") === 0 && get(group, "playposition") === 0.35,
        surface + " playing CUE did not return and stop");
    D2.cueButton(0, 0x70, 0, 0, surface);

    /* Main CUE operations never overwrite Hot Cue A. */
    assert(get(group, "hotcue_1_position") === 70000 &&
           get(group, "hotcue_1_enabled") === 1,
        surface + " Main Cue changed Hot Cue A");
}

runSurface("[Channel1]", 1);
runSurface("[Channel2]", 2);
assert(requests.filter(function(request) {
    return request[1] === "cue_cdj";
}).length === 16, "unexpected native cue_cdj press/release count");

console.log("D2_PIONEER_CUE_OK surfaces=2 scenarios=12");
