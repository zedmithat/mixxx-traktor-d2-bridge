const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

function fixture(zed = false, lfoAvailable = true) {
    const values = new Map();
    const writes = [];
    const queue = [];
    const feedback = [];
    const timers = new Map();
    let now = 10000;
    let timerId = 0;
    const engine = {
        getValue(group, control) {
            if (values.has(group+'/'+control)) return values.get(group+'/'+control);
            return control === 'num_effectslots' ? 3 : 0;
        },
        setValue(group, control, value) {
            writes.push([group, control, value]);
            if (control === 'loaded_effect' || control === 'clear' || control === 'enabled') {
                queue.push(() => {
                    values.set(group+'/'+control, value);
                    if (control === 'loaded_effect') values.set(group+'/loaded', 1);
                    if (control === 'clear' && value) {
                        for (const key of values.keys()) if (key.startsWith(group+'/')) values.delete(key);
                    }
                });
            } else values.set(group+'/'+control, value);
        },
        setParameter(group, control, value) {
            assert.ok(value >= 0 && value <= 1);
            writes.push([group, control, value]);
            values.set(group+'/'+control, value);
        },
        beginTimer(interval, callback) {
            if (interval === 50 && !lfoAvailable) return 0;
            timers.set(++timerId, callback);
            return timerId;
        },
        stopTimer(identifier) { timers.delete(identifier); },
        makeConnection() { return {isConnected: true, disconnect() {}}; }
    };
    const context = vm.createContext({engine, midi: {sendShortMsg: (...message) => feedback.push(message)},
        print() {}, Date: {now: () => now}});
    for (const file of ['TouchFX-scripts.js', ...(zed ? ['integration/zed-extension.js'] : []), 'TouchFX-macros.js']) {
        vm.runInContext(fs.readFileSync(path.join(__dirname, '..', file), 'utf8'), context);
    }
    const mapping = context.TouchFX;
    mapping.init();
    context.TouchFXRouter.init();
    function flush() { while (queue.length) queue.shift()(); }
    function tick(milliseconds = 250) { now += milliseconds; mapping.watchdog(); }
    function send(control, value, channel = 0) { mapping.macroMessage(channel, control, value, 0xB0+channel); }
    function select(id, channel = 0) {
        for (const [control, value] of [[16, id], [20, 14], [21, mapping.macroSlots[id] === 2 ? 15 : 0], [22, id]]) send(control, value, channel);
    }
    function settle() { for (let cycle = 0; cycle < 3; cycle++) { flush(); tick(); } flush(); }
    flush();
    return {mapping, router: context.TouchFXRouter, values, writes, feedback, timers, engine, flush, tick, send, select, settle,
        get: (slot, control) => engine.getValue(slot ? mapping.effectSlot(slot) : mapping.unit, control)};
}

for (let mode = 1; mode <= 11; mode++) {
    test('macro '+mode+' waits for actual slot readiness and routes XY on four decks', () => {
        const state = fixture();
        state.select(mode);
        state.mapping.gate(0, 60, 127, 0x90);
        state.flush();
        assert.equal(state.get(0, 'enabled'), 0);
        state.tick();
        assert.equal(state.mapping.macroBusy, true);
        state.settle();
        assert.equal(state.mapping.macroId, mode);
        assert.equal(state.mapping.macroBusy, false);
        assert.equal(state.get(1, 'enabled'), 1);
        assert.equal(state.get(2, 'enabled'), state.mapping.macroSlots[mode] === 2 ? 1 : 0);
        for (let channel = 0; channel < 4; channel++) {
            state.mapping.axis(channel, 10, 100, 0xB0+channel);
            state.mapping.axis(channel, 11, 90, 0xB0+channel);
            state.mapping.gate(channel, 60, 127, 0x90+channel);
            state.flush();
            assert.equal(state.get(0, 'mix'), 1);
            assert.equal(state.get(0, 'enabled'), 1);
            for (let deck = 1; deck <= 4; deck++) assert.equal(state.get(0, 'group_[Channel'+deck+']_enable'), deck === channel+1 ? 1 : 0);
            state.mapping.gate(channel, 60, 0, 0x80+channel);
            state.flush();
            assert.equal(state.get(0, 'enabled'), 0);
        }
        assert.ok(state.feedback.some(message => message[1] === 18 && message[2] === mode));
    });
}

test('wrong channel, incomplete commit and timeout never arm a macro', () => {
    const state = fixture();
    state.send(16, 1);
    state.send(20, 14, 1);
    state.send(21, 15);
    state.send(22, 1);
    assert.equal(state.mapping.macroPending, null);
    state.tick(1600);
    state.mapping.gate(0, 60, 127, 0x90);
    state.flush();
    assert.equal(state.mapping.macroBusy, true);
    assert.equal(state.get(0, 'enabled'), 0);
    state.select(11);
    state.settle();
    assert.equal(state.mapping.macroBusy, false);
});

test('delayed clear and same-index Reset cannot acknowledge old parameters', () => {
    const state = fixture();
    state.select(6);
    state.settle();
    state.send(24, 127);
    state.mapping.axis(0, 11, 127, 0xB0);
    state.mapping.gate(0, 60, 127, 0x90);
    state.flush();
    assert.equal(state.get(2, 'parameter2'), 0.75);
    state.send(26, 127);
    state.tick();
    assert.equal(state.mapping.macroBusy, true);
    assert.equal(state.mapping.macroPending.clearing, true);
    state.settle();
    assert.equal(state.get(0, 'enabled'), 0);
    assert.equal(state.get(0, 'mix'), 0);
    assert.equal(state.get(2, 'parameter2'), 0.35+0.4*64/127);
    assert.equal(state.mapping.macroBusy, false);
});

test('Amount caps Dub feedback and Noise level; stale deck cannot reset', () => {
    const state = fixture();
    state.select(6);
    state.settle();
    state.send(24, 127);
    assert.equal(state.get(2, 'parameter2'), 0.75);
    assert.equal(state.get(2, 'parameter4'), 0.5);
    state.select(8);
    state.settle();
    state.send(24, 127);
    assert.equal(state.get(1, 'parameter1'), 0.08);
    state.send(26, 127, 1);
    assert.equal(state.mapping.macroBusy, false);
    state.send(24, 0);
    assert.equal(state.get(1, 'parameter1'), 0);
});

test('LFO is bounded, active-only, watchdog guarded and timer failure blocks it', () => {
    const state = fixture();
    state.select(10);
    state.settle();
    state.writes.length = 0;
    state.mapping.lfoTick();
    assert.equal(state.writes.length, 0);
    state.mapping.gate(0, 60, 127, 0x90);
    state.flush();
    state.tick(100);
    state.mapping.lfoTick();
    assert.ok(state.get(1, 'meta') >= 0 && state.get(1, 'meta') <= 1);
    state.tick(1600);
    state.flush();
    assert.equal(state.get(0, 'enabled'), 0);
    state.router.shutdown();
    state.mapping.shutdown();
    assert.equal(state.timers.size, 0);
    const unavailable = fixture(false, false);
    unavailable.select(5);
    unavailable.settle();
    assert.equal(unavailable.mapping.macroBusy, true);
    assert.equal(unavailable.get(0, 'enabled'), 0);
});

test('Filter Echo separates cutoff, exact delay beats and feedback', () => {
    const state = fixture(true);
    state.select(1);
    state.settle();
    for (const [position, beats, delay] of [[0, 2, 2], [22, 1, 1], [43, 0.75, 0.75],
        [64, 0.5, 0.5], [86, 0.25, 0.25], [127, 0.125, 0]]) {
        state.mapping.axis(0, 10, 100, 0xB0);
        state.mapping.axis(0, 11, position, 0xB0);
        assert.equal(state.get(1, 'meta'), 100/127);
        assert.equal(state.get(2, 'parameter1'), delay);
        assert.equal(state.mapping.beatStep('echo', position/127), beats);
        assert.equal(state.get(2, 'button_parameter1'), 1);
        assert.equal(state.get(2, 'button_parameter2'), 0);
    }
    state.send(24, 127);
    assert.equal(state.get(2, 'parameter2'), 0.55);
    assert.equal(state.get(2, 'parameter4'), 0.35);
    assert.equal(state.get(0, 'enabled'), 0);
    assert.ok(!state.writes.some(([group]) => /EffectUnit[12]/.test(group)));
});

test('all macro controls use the declared effect parameters and bounded extra slider', () => {
    const expected = {
        2: [[1, 'parameter1', 0.9], [2, 'parameter2', 1], [1, 'parameter4', 0.65]],
        3: [[1, 'meta', 1], [2, 'parameter1', 0.9], [2, 'parameter4', 0.65]],
        4: [[1, 'meta', 1], [2, 'parameter1', 0], [1, 'parameter2', 2.5]],
        5: [[1, 'parameter4', 0.65], [1, 'parameter2', 0.55]],
        6: [[1, 'meta', 1], [2, 'parameter1', 0], [2, 'parameter2', 0.75]],
        7: [[1, 'meta', 1], [2, 'parameter2', 8], [2, 'parameter1', 1]],
        8: [[2, 'parameter2', 8], [2, 'parameter1', 1], [1, 'parameter1', 0.08]],
        9: [[1, 'parameter1', 0.5], [1, 'parameter2', 1], [1, 'parameter4', 0.5]],
        10: [[1, 'meta', 1]],
        11: [[1, 'meta', 1], [1, 'parameter2', 2.5]]
    };
    for (const [identifier, controls] of Object.entries(expected)) {
        const state = fixture();
        state.select(Number(identifier));
        state.settle();
        state.mapping.axis(0, 10, 127, 0xB0);
        state.mapping.axis(0, 11, 127, 0xB0);
        state.send(24, 127);
        for (const [slot, control, value] of controls) assert.equal(state.get(slot, control), value, identifier+' '+control);
        assert.equal(state.get(0, 'enabled'), 0, 'parameters must not arm the gate');
    }
});

test('gate period is converted to cycles per beat, not a normalized knob value', () => {
    const state = fixture();
    state.select(7);
    state.settle();
    for (const [position, expected] of [[0, 0.25], [22, 0.5], [43, 1], [64, 2], [86, 4], [127, 8]]) {
        state.mapping.axis(0, 11, position, 0xB0);
        assert.equal(state.get(2, 'parameter2'), expected);
    }
    assert.equal(state.get(2, 'button_parameter1'), 1);
    assert.equal(state.get(2, 'button_parameter2'), 0);
});

test('ZED macro-to-single transition disables slot 2 and never writes D2 units', () => {
    const state = fixture(true);
    state.select(1);
    state.settle();
    state.mapping.selectEffect(0, 13, 3, 0xB0);
    state.settle();
    assert.equal(state.mapping.macroId, 0);
    assert.equal(state.get(2, 'enabled'), 0);
    assert.equal(state.get(1, 'loaded_effect'), 3);
    assert.ok(!state.writes.some(([group]) => /EffectUnit[12]/.test(group)));
    state.select(6, 3);
    assert.equal(state.mapping.macroId, 0);
});

test('standalone engagement feedback reports engine state rather than sent state', () => {
    const state = fixture();
    state.select(11);
    state.settle();
    state.mapping.gate(0, 60, 127, 0x90);
    state.tick();
    assert.equal(state.feedback.filter(message => message[1] === 15).at(-1)[2] & 1, 0);
    state.flush();
    state.tick();
    assert.equal(state.feedback.filter(message => message[1] === 15).at(-1)[2] & 1, 1);
});

test('mapping reload cannot leave a persisted generator or old chain armed', () => {
    const state = fixture(true);
    state.select(8);
    state.settle();
    assert.equal(state.get(1, 'enabled'), 1);
    state.router.shutdown();
    state.mapping.shutdown();
    state.mapping.init();
    state.router.init();
    state.flush();
    assert.equal(state.mapping.macroId, 0);
    for (let slot = 0; slot <= 3; slot++) assert.equal(state.get(slot, 'enabled'), 0);
});
