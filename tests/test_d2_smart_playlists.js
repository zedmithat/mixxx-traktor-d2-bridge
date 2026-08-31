"use strict";

var fs = require("fs");
var vm = require("vm");
var values = {};
var calls = [];
var sysex = [];

function key(group, control) { return group + "|" + control; }
function assert(condition, message) { if (!condition) throw new Error(message); }

global.engine = {
    getTrackLocation: function() { return ""; },
    getValue: function(group, control) {
        if (values[key(group, control)] !== undefined)
            return values[key(group, control)];
        if (control === "visual_bpm") return 125;
        if (control === "bpm") return 125;
        if (control === "visual_key") return 20;
        if (control === "rate_ratio") return 1;
        return 0;
    },
    setValue: function(group, control, value) {
        values[key(group, control)] = value;
        calls.push([group, control, value]);
    },
    getParameter: function() { return 0; },
    setParameter: function() {},
    makeConnection: function() { return {disconnect: function() {}}; },
    beginTimer: function(interval, callback) { return 1; },
    stopTimer: function() {},
    scratchEnable: function() {},
    scratchTick: function() {},
    scratchDisable: function() {},
    isScratching: function() { return false; }
};
global.midi = {
    sendSysexMsg: function(message) {
        sysex.push(message.slice(2, message.length - 1).map(function(byte) {
            return String.fromCharCode(byte);
        }).join(""));
    }
};

vm.runInThisContext(fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2-scripts.js", "utf8"));

var group = "[Channel1]";
values[key("[Tab]", "current")] = D2.skinPage.browse;
values[key(group, "visual_bpm")] = 125;
values[key(group, "visual_key")] = 20;

D2.rightScreenButton(0, 0x35, 0x7F, 0, group);
assert(D2.smartMenu[group], "Browse R1 did not open Smart Lists");
assert(sysex.indexOf("D2|1|SMARTMENU|1,0") >= 0,
    "Smart Lists open state was not sent to the display bridge");

D2.browseEncoder(0, 0x14, 0x41, 0, group);
assert(D2.smartIndex[group] === 1, "Smart Lists clockwise selection failed");
D2.browseEncoder(0, 0x14, 0x3F, 0, group);
assert(D2.smartIndex[group] === 0, "Smart Lists counter-clockwise selection failed");

var beforeActivation = calls.length;
D2.sidebarActivate(0, 0x3D, 0x7F, 0, group);
var activation = calls.slice(beforeActivation);
function called(control, value) {
    return activation.some(function(call) {
        return call[0] === "[Library]" && call[1] === control &&
            (value === undefined || call[2] === value);
    });
}
assert(called("d2_smart_bpm", 125), "Active deck BPM was not published");
assert(called("d2_smart_key", 20), "Active deck key was not published");
assert(called("d2_smart_list", 1), "MATCH CURRENT DECK was not activated");
assert(!D2.smartMenu[group], "Smart Lists did not close after selection");
assert(!activation.some(function(call) { return call[1] === "d2_sidebar_activate"; }),
    "Smart selection leaked into native sidebar activation");

D2.rightScreenButton(0, 0x35, 0x7F, 0, group);
var beforeBack = calls.length;
D2.backButton(0, 0x3F, 0x7F, 0, group);
assert(!D2.smartMenu[group], "Back did not close Smart Lists");
assert(!calls.slice(beforeBack).some(function(call) {
    return call[0] === "[Library]" && call[1] === "focused_widget";
}), "Closing Smart Lists unexpectedly changed native Library focus");

console.log("D2_SMART_PLAYLISTS_JS_TEST_OK presets=6 bpm=125 key=20");
