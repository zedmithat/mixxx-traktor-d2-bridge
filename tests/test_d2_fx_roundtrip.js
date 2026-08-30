"use strict";

/*
 * Deterministic contract test for the shared Mixxx FX Control Objects.
 *
 * The skin and controller must not keep independent selected-effect state.
 * D2 asks Mixxx to advance an effect with next_effect, then waits until
 * Mixxx confirms the authoritative 1-indexed loaded_effect value.  Only that
 * confirmation is transported to the D2 renderer as FXSEL1..3.
 */

var assert = require("node:assert/strict");
var fs = require("node:fs");
var vm = require("node:vm");

var values = {};
var parameters = {};
var calls = [];
var sysex = [];
var connections = [];
var timers = [];
var scratching = {};
var effectNames = {};

function key(group, control) {
    return group + "|" + control;
}

function effectUnitGroup(unit) {
    return "[EffectRack1_EffectUnit" + unit + "]";
}

function effectGroup(unit, slot) {
    return "[EffectRack1_EffectUnit" + unit + "_Effect" + slot + "]";
}

function decodeSysex(message) {
    return message.slice(2, message.length - 1).map(function(byte) {
        return String.fromCharCode(byte);
    }).join("");
}

function sysexSince(start) {
    return sysex.slice(start).map(decodeSysex);
}

function fxSelectionMessagesSince(start) {
    return sysexSince(start).filter(function(text) {
        return /^D2\|[12]\|FXSEL[123]\|/.test(text);
    });
}

function fxNameMessagesSince(start) {
    return sysexSince(start).filter(function(text) {
        return /^D2\|[12]\|FXNAME[123]\|U:/.test(text);
    });
}

function callsSince(start) {
    return calls.slice(start);
}

function writesTo(group, control, start) {
    return callsSince(start || 0).filter(function(call) {
        return call.group === group && call.control === control;
    });
}

function matchingConnections(group, control) {
    return connections.filter(function(connection) {
        return connection.group === group && connection.control === control;
    });
}

function oneConnection(group, control) {
    var matches = matchingConnections(group, control);
    assert.equal(matches.length, 1,
        group + " " + control + " must have exactly one feedback owner");
    return matches[0];
}

/* Simulate a value confirmed by Mixxx. Controller writes intentionally do not
 * auto-trigger callbacks in this harness, so request and confirmation cannot
 * accidentally collapse into one false-positive operation. */
function emitValue(group, control, value) {
    values[key(group, control)] = value;
    var connection = oneConnection(group, control);
    connection.callback(value, group, control);
}

function emitParameter(group, control, value) {
    parameters[key(group, control)] = value;
    var connection = oneConnection(group, control);
    connection.callback(value, group, control);
}

global.engine = {
    getTrackLocation: function(group) {
        return group === "[Channel1]" ? "/test/deck-a.mp3" : "/test/deck-b.mp3";
    },
    getEffectName: function(group) {
        if (effectNames[group] !== undefined) return effectNames[group];
        var selected = values[key(group, "loaded_effect")] || 0;
        return selected ? "Effect " + selected : "";
    },
    getValue: function(group, control) {
        if (values[key(group, control)] !== undefined)
            return values[key(group, control)];
        if (control === "duration") return 420;
        if (control === "bpm") return 128;
        if (control === "rate_ratio") return 1;
        if (control === "beat_prev") return 100;
        if (control === "beat_next") return 200;
        if (control === "track_samplerate") return 44100;
        if (control === "track_id") return group === "[Channel1]" ? 101 : 202;
        if (control === "beatloop_size") return 4;
        if (control.indexOf("hotcue_") === 0 && control.indexOf("_position") > 0)
            return -1;
        if (control.indexOf("browse_track_id_") === 0) return 1;
        return 0;
    },
    setValue: function(group, control, value) {
        values[key(group, control)] = value;
        calls.push({type: "value", group: group, control: control, value: value});
    },
    getParameter: function(group, control) {
        return parameters[key(group, control)] === undefined ?
            0 : parameters[key(group, control)];
    },
    setParameter: function(group, control, value) {
        parameters[key(group, control)] = value;
        calls.push({type: "parameter", group: group, control: control, value: value});
    },
    makeConnection: function(group, control, callback) {
        var connection = {
            group: group,
            control: control,
            callback: callback,
            disconnected: false
        };
        connections.push(connection);
        return {
            disconnect: function() {
                connection.disconnected = true;
            }
        };
    },
    beginTimer: function(interval, callback, oneShot) {
        var timer = {interval: interval, callback: callback, oneShot: !!oneShot};
        timers.push(timer);
        return timers.length;
    },
    stopTimer: function() {},
    scratchEnable: function(deck) { scratching[deck] = true; },
    scratchTick: function() {},
    scratchDisable: function(deck) { scratching[deck] = false; },
    isScratching: function(deck) { return !!scratching[deck]; }
};

global.midi = {
    sendSysexMsg: function(message, length) {
        assert.equal(message.length, length, "invalid SysEx length");
        assert.equal(message[0], 0xF0, "invalid SysEx start");
        assert.equal(message[length - 1], 0xF7, "invalid SysEx end");
        sysex.push(message);
    }
};

/* Opaque numeric values on purpose: loaded_effect indexes the user-configurable
 * Visible Effects List. Human labels come independently from Mixxx's live
 * EffectManifest snapshot and must never be hard-coded from these indexes. */
for (var seedUnit = 1; seedUnit <= 2; seedUnit++) {
    values[key(effectUnitGroup(seedUnit),
        "group_[Channel" + seedUnit + "]_enable")] = 1;
    for (var seedSlot = 1; seedSlot <= 3; seedSlot++) {
        values[key(effectGroup(seedUnit, seedSlot), "loaded_effect")] =
            seedUnit * 10 + seedSlot;
    }
}

vm.runInThisContext(fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2-scripts.js", "utf8"), {
    filename: "Traktor-Kontrol-D2-scripts.js"
});

assert.equal(D2.fxNameWireValue("Echo | 50%"), "U:Echo%20%7C%2050%25");
assert.equal(D2.fxNameWireValue("Çığlık"),
    "U:%C3%87%C4%B1%C4%9Fl%C4%B1k");
assert.equal(D2.fxNameWireValue("A".repeat(64)), "U:" + "A".repeat(63));
assert.equal(D2.fxNameWireValue("é".repeat(32)),
    "U:" + "%C3%A9".repeat(31));
assert.equal(D2.fxNameWireValue("😀".repeat(16)),
    "U:" + "%F0%9F%98%80".repeat(15));
assert.equal(D2.fxNameWireValue("\uD800"), "U:%EF%BF%BD");
assert.equal(D2.fxNameWireValue(""), "U:");

D2.init("fx-roundtrip-test", false);

assert.equal(D2.fxUnit["[Channel1]"], 1, "D2 surface 1 must start on FX Unit 1");
assert.equal(D2.fxUnit["[Channel2]"], 2, "D2 surface 2 must start on FX Unit 2");

/* Every mutable value exposed by the two deck FX bars has one controller
 * feedback connection. This is what makes main-screen changes reach D2
 * without waiting for the 500 ms resilience snapshot. */
var expectedFxConnections = [];
for (var unit = 1; unit <= 2; unit++) {
    expectedFxConnections.push([effectUnitGroup(unit), "mix"]);
    expectedFxConnections.push([effectUnitGroup(unit), "group_[Channel1]_enable"]);
    expectedFxConnections.push([effectUnitGroup(unit), "group_[Channel2]_enable"]);
    for (var slot = 1; slot <= 3; slot++) {
        expectedFxConnections.push([effectGroup(unit, slot), "meta"]);
        expectedFxConnections.push([effectGroup(unit, slot), "enabled"]);
        expectedFxConnections.push([effectGroup(unit, slot), "loaded_effect"]);
    }
}
assert.equal(expectedFxConnections.length, 24);
expectedFxConnections.forEach(function(pair) {
    oneConnection(pair[0], pair[1]);
});
assert.equal(D2.fxStateConnections.length, expectedFxConnections.length,
    "FX feedback ownership must be complete and duplicate-free");

/* Main screen -> CO -> D2 transport. Feedback callbacks are observers: they
 * may publish SysEx/LED state, but must never write another CO and form an
 * echo loop. */
var callStart = calls.length;
var sysexStart = sysex.length;
emitParameter(effectUnitGroup(1), "mix", 0.375);
assert.equal(calls.length, callStart, "mix feedback wrote back to a CO");
assert.deepEqual(fxSelectionMessagesSince(sysexStart), [],
    "mix feedback masqueraded as an effect-selection confirmation");
assert.ok(sysexSince(sysexStart).includes("D2|1|FX1|0.3750"),
    "Unit 1 mix did not reach its owning D2 surface");
assert.ok(!sysexSince(sysexStart).includes("D2|2|FX1|0.3750"),
    "Unit 1 mix leaked to a D2 surface that owns Unit 2");

callStart = calls.length;
sysexStart = sysex.length;
emitParameter(effectGroup(2, 2), "meta", 0.625);
assert.equal(calls.length, callStart, "meta feedback wrote back to a CO");
assert.ok(sysexSince(sysexStart).includes("D2|2|FX3|0.6250"),
    "Unit 2 slot 2 meta did not reach its owning D2 surface");
assert.ok(!sysexSince(sysexStart).includes("D2|1|FX3|0.6250"),
    "Unit 2 slot 2 meta leaked to the Unit 1 surface");

callStart = calls.length;
sysexStart = sysex.length;
emitValue(effectGroup(1, 3), "enabled", 1);
assert.equal(calls.length, callStart, "enabled feedback wrote back to a CO");
assert.ok(sysexSince(sysexStart).includes("D2|1|FXEN3|1"),
    "Unit 1 enabled state did not reach its owning D2 surface");
assert.ok(!sysexSince(sysexStart).includes("D2|2|FXEN3|1"),
    "Unit 1 enabled state leaked to the Unit 2 surface");

var surfaces = ["[Channel1]", "[Channel2]"];

/* The four physical knobs keep the approved parameter layout: knob 0 is the
 * fixed surface unit's mix, and knobs 1..3 are that unit's slot meta values. */
surfaces.forEach(function(surface, surfaceIndex) {
    var ownedUnit = surfaceIndex + 1;
    for (var knob = 0; knob <= 3; knob++) {
        var midiValue = 31 + knob * 17;
        var expectedParameter = midiValue / 127;
        var targetGroup = knob === 0 ?
            effectUnitGroup(ownedUnit) : effectGroup(ownedUnit, knob);
        var targetControl = knob === 0 ? "mix" : "meta";
        callStart = calls.length;
        D2.fxKnob(0, 0x24 + knob, midiValue, 0, surface);
        var knobWrites = writesTo(targetGroup, targetControl, callStart);
        assert.equal(knobWrites.length, 1,
            surface + " knob " + knob + " did not write " + targetControl);
        assert.equal(knobWrites[0].type, "parameter",
            surface + " knob " + knob + " did not use setParameter");
        assert.ok(Math.abs(knobWrites[0].value - expectedParameter) < 0.0000001,
            surface + " knob " + knob + " parameter scaling is wrong");
        assert.equal(callsSince(callStart).length, 1,
            surface + " knob " + knob + " wrote outside its fixed FX unit");
    }
});

/* Button 0 owns the surface unit -> own deck route. SHIFT deliberately does
 * not alter this behavior. It must never toggle an effect slot or select one. */
surfaces.forEach(function(surface, surfaceIndex) {
    var ownedUnit = surfaceIndex + 1;
    var unitGroup = effectUnitGroup(ownedUnit);
    var ownRoute = "group_[Channel" + ownedUnit + "]_enable";
    var otherRoute = "group_[Channel" + (ownedUnit === 1 ? 2 : 1) + "]_enable";
    [false, true].forEach(function(shifted) {
        D2.shiftButton(0, 0x5A, shifted ? 0x7F : 0, 0, surface);
        values[key(unitGroup, ownRoute)] = 0;
        callStart = calls.length;
        D2.fxButton(0, 0x28, 0x7F, 0, surface);
        assert.deepEqual(writesTo(unitGroup, ownRoute, callStart).map(function(call) {
            return call.value;
        }), [true], surface + " button 0 did not toggle its own deck route" +
            (shifted ? " while SHIFT was held" : ""));
        assert.equal(writesTo(unitGroup, otherRoute, callStart).length, 0,
            surface + " button 0 changed the other deck route");
        assert.ok(!callsSince(callStart).some(function(call) {
            return call.control === "enabled" || call.control === "next_effect" ||
                call.control === "loaded_effect";
        }), surface + " button 0 changed a slot instead of its route");

        var routeReleaseStart = calls.length;
        D2.fxButton(0, 0x28, 0, 0, surface);
        assert.equal(calls.length, routeReleaseStart,
            surface + " button 0 release was not a no-op");
    });
    D2.shiftButton(0, 0x5A, 0, 0, surface);
});

/* Normal buttons 1..3 toggle slots 1..3 enabled only. Button release is a
 * no-op. The MIX/route button at index 0 is intentionally not a slot. */
surfaces.forEach(function(surface, surfaceIndex) {
    var ownedUnit = surfaceIndex + 1;
    D2.shiftButton(0, 0x5A, 0, 0, surface);
    for (var normalSlot = 1; normalSlot <= 3; normalSlot++) {
        var targetGroup = effectGroup(ownedUnit, normalSlot);
        values[key(targetGroup, "enabled")] = 0;
        callStart = calls.length;
        D2.fxButton(0, 0x28 + normalSlot, 0x7F, 0, surface);
        var normalWrites = callsSince(callStart);
        assert.deepEqual(writesTo(targetGroup, "enabled", callStart).map(function(call) {
            return call.value;
        }), [true], surface + " normal button " + normalSlot +
            " did not toggle slot " + normalSlot + " enabled");
        assert.ok(!normalWrites.some(function(call) {
            return call.control === "next_effect" || call.control === "loaded_effect";
        }), surface + " normal button " + normalSlot + " changed effect selection");

        var releaseCallStart = calls.length;
        D2.fxButton(0, 0x28 + normalSlot, 0, 0, surface);
        assert.equal(calls.length, releaseCallStart,
            surface + " button " + normalSlot + " release was not a no-op");
    }
});

/* SHIFT+buttons 1..3 request one authoritative next_effect pulse for slots
 * 1..3. No enabled or loaded_effect write is allowed, and no FXSEL may be
 * emitted before Mixxx's separate loaded_effect confirmation callback. */
surfaces.forEach(function(surface, surfaceIndex) {
    var ownedUnit = surfaceIndex + 1;
    D2.shiftButton(0, 0x5A, 0x7F, 0, surface);
    for (var shiftedSlot = 1; shiftedSlot <= 3; shiftedSlot++) {
        var targetGroup = effectGroup(ownedUnit, shiftedSlot);
        var oldLoaded = values[key(targetGroup, "loaded_effect")];
        callStart = calls.length;
        sysexStart = sysex.length;
        D2.fxButton(0, 0x28 + shiftedSlot, 0x7F, 0, surface);

        assert.deepEqual(writesTo(targetGroup, "next_effect", callStart).map(function(call) {
            return call.value;
        }), [1, 0], surface + " SHIFT+button " + shiftedSlot +
            " did not pulse next_effect 1 -> 0");
        assert.equal(writesTo(targetGroup, "enabled", callStart).length, 0,
            surface + " SHIFT+button " + shiftedSlot + " toggled enabled");
        assert.equal(writesTo(targetGroup, "loaded_effect", callStart).length, 0,
            surface + " SHIFT+button " + shiftedSlot + " bypassed Mixxx ownership");
        assert.deepEqual(fxSelectionMessagesSince(sysexStart), [],
            surface + " SHIFT+button " + shiftedSlot +
            " published an unconfirmed selection");
        assert.equal(values[key(targetGroup, "loaded_effect")], oldLoaded,
            "request mutated loaded_effect before Mixxx confirmation");

        var confirmed = oldLoaded + 30 + shiftedSlot;
        callStart = calls.length;
        var confirmationSysexStart = sysex.length;
        emitValue(targetGroup, "loaded_effect", confirmed);
        assert.equal(calls.length, callStart,
            "loaded_effect confirmation wrote back to a CO");
        var expectedPayload = "D2|" + (surfaceIndex + 1) + "|FXSEL" +
            shiftedSlot + "|" + confirmed;
        var otherPayloadPrefix = "D2|" + (surfaceIndex === 0 ? 2 : 1) +
            "|FXSEL" + shiftedSlot + "|";
        var confirmationMessages = fxSelectionMessagesSince(confirmationSysexStart);
        assert.ok(confirmationMessages.includes(expectedPayload),
            surface + " did not receive Mixxx's loaded_effect confirmation");
        assert.ok(!confirmationMessages.some(function(text) {
            return text.indexOf(otherPayloadPrefix) === 0;
        }), "loaded_effect confirmation leaked to a surface owning another unit");
        var expectedName = "D2|" + (surfaceIndex + 1) + "|FXNAME" +
            shiftedSlot + "|U:Effect%20" + confirmed;
        var confirmationNames = fxNameMessagesSince(confirmationSysexStart);
        assert.ok(confirmationNames.includes(expectedName),
            surface + " did not receive the authoritative EffectManifest name");
        assert.ok(!confirmationNames.some(function(text) {
            return text.indexOf("D2|" + (surfaceIndex === 0 ? 2 : 1) +
                "|FXNAME" + shiftedSlot + "|") === 0;
        }), "effect name leaked to a surface owning another unit");

        var shiftedReleaseStart = calls.length;
        D2.fxButton(0, 0x28 + shiftedSlot, 0, 0, surface);
        assert.equal(calls.length, shiftedReleaseStart,
            surface + " shifted button release was not a no-op");
    }
    D2.shiftButton(0, 0x5A, 0, 0, surface);
});

/* SHIFT is surface-local. Holding it on D2 1 must not turn a normal D2 2
 * press into an effect-selection request. */
D2.shiftButton(0, 0x5A, 0x7F, 0, "[Channel1]");
D2.shiftButton(0, 0x5A, 0, 0, "[Channel2]");
values[key(effectGroup(2, 1), "enabled")] = 0;
callStart = calls.length;
D2.fxButton(0, 0x29, 0x7F, 0, "[Channel2]");
assert.deepEqual(writesTo(effectGroup(2, 1), "enabled", callStart).map(function(call) {
    return call.value;
}), [true], "D2 1 SHIFT state leaked into D2 2");
assert.equal(writesTo(effectGroup(2, 1), "next_effect", callStart).length, 0,
    "D2 2 normal press inherited D2 1 SHIFT selection mode");
D2.shiftButton(0, 0x5A, 0, 0, "[Channel1]");

/* FX SELECT cannot change ownership. Repeated presses and releases leave D2 1
 * on Unit 1 and D2 2 on Unit 2, and never write an FX Control Object. */
surfaces.forEach(function(surface, surfaceIndex) {
    var ownedUnit = surfaceIndex + 1;
    var expectedVisible = false;
    [0x7F, 0, 0x7F].forEach(function(value) {
        callStart = calls.length;
        sysexStart = sysex.length;
        D2.fxSelectButton(0, 0x30, value, 0, surface);
        if (value) expectedVisible = !expectedVisible;
        assert.equal(D2.fxUnit[surface], ownedUnit,
            surface + " FX SELECT changed its fixed Unit " + ownedUnit + " ownership");
        assert.equal(D2.fxSettingsVisible[surface], expectedVisible,
            surface + " FX SELECT did not toggle its settings overlay");
        assert.equal(D2.effectUnitGroup(surface), effectUnitGroup(ownedUnit),
            surface + " FX SELECT redirected the unit Control Object group");
        assert.equal(calls.length, callStart,
            surface + " FX SELECT wrote an FX Control Object");
        if (value) {
            assert.ok(sysexSince(sysexStart).includes(
                "D2|" + ownedUnit + "|FXTOUCH|" +
                    (expectedVisible ? "15" : "0")),
                surface + " FX SELECT did not publish its settings view");
        }
    });
});

/* An FX SELECT attempt must not change confirmation fan-out: Unit 2 remains
 * visible only on D2 2, never on D2 1. */
callStart = calls.length;
sysexStart = sysex.length;
emitValue(effectGroup(2, 1), "loaded_effect", 99);
assert.equal(calls.length, callStart, "fixed-unit confirmation wrote to a CO");
var fixedOwnershipMessages = fxSelectionMessagesSince(sysexStart);
assert.ok(fixedOwnershipMessages.includes("D2|2|FXSEL1|99"),
    "Unit 2 confirmation did not reach D2 2");
assert.ok(!fixedOwnershipMessages.includes("D2|1|FXSEL1|99"),
    "Unit 2 confirmation leaked to fixed Unit 1 surface");

/* A numeric selection change clears the bridge label first. Even when two
 * manifests share the same display name, the name packet must follow again
 * instead of being swallowed by the JS state cache. */
var duplicateNameGroup = effectGroup(1, 1);
effectNames[duplicateNameGroup] = "Echo";
emitValue(duplicateNameGroup, "loaded_effect", 110);
sysexStart = sysex.length;
emitValue(duplicateNameGroup, "loaded_effect", 111);
assert.ok(fxNameMessagesSince(sysexStart).includes(
    "D2|1|FXNAME1|U:Echo"),
    "same-name selection change did not republish the cleared label");

/* SHIFT selection remains on the physical surface's immutable unit even after
 * FX SELECT was pressed. Button 3 owns slot 3. */
D2.shiftButton(0, 0x5A, 0x7F, 0, "[Channel1]");
callStart = calls.length;
D2.fxButton(0, 0x2B, 0x7F, 0, "[Channel1]");
assert.deepEqual(writesTo(effectGroup(1, 3), "next_effect", callStart).map(function(call) {
    return call.value;
}), [1, 0], "D2 1 SHIFT+button 3 did not stay on fixed Unit 1 slot 3");
assert.equal(writesTo(effectGroup(2, 3), "next_effect", callStart).length, 0,
    "D2 1 SHIFT+button 3 wrote to Unit 2");
D2.shiftButton(0, 0x5A, 0, 0, "[Channel1]");

/* Physical FX LEDs use the same four columns as the controls:
 * Unit/master route, slot 1, slot 2, slot 3. */
surfaces.forEach(function(surface, surfaceIndex) {
    var ownedUnit = surfaceIndex + 1;
    values[key(effectUnitGroup(ownedUnit),
        "group_[Channel" + ownedUnit + "]_enable")] = 1;
    values[key(effectGroup(ownedUnit, 1), "enabled")] = 1;
    values[key(effectGroup(ownedUnit, 2), "enabled")] = 0;
    values[key(effectGroup(ownedUnit, 3), "enabled")] = 1;
    D2.sentState = {};
    sysexStart = sysex.length;
    D2.refreshLEDs(surface);
    var ledPackets = sysexSince(sysexStart).filter(function(text) {
        return text.indexOf("D2|" + (surfaceIndex + 1) + "|LEDPACK|") === 0;
    });
    assert.equal(ledPackets.length, 1,
        surface + " did not publish one FX LED snapshot");
    var ledFields = ledPackets[0].split("|")[3].split(",");
    assert.equal(Number(ledFields[8]), ownedUnit,
        surface + " LED snapshot reported the wrong fixed FX unit");
    assert.equal(Number(ledFields[9]), 0x0B,
        surface + " FX LED columns are not route,slot1,slot2,slot3");
});

var trackedFxConnections = expectedFxConnections.map(function(pair) {
    return oneConnection(pair[0], pair[1]);
});
D2.shutdown();
assert.ok(trackedFxConnections.every(function(connection) {
    return connection.disconnected;
}), "shutdown left FX feedback connections alive");

console.log("D2_FX_ROUNDTRIP_TEST_OK connections=" + expectedFxConnections.length +
    " sysex=" + sysex.length + " calls=" + calls.length);
