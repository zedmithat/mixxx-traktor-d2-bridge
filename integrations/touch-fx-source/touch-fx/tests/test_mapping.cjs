const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

function fixture(timerAvailable = true, zed = false, effectUnits = 4) {
    const values = new Map();
    const writes = [];
    const timers = new Map();
    let now = 10000;
    let nextTimer = 1;
    const requests = [];
    const connections = [];
    const skin = {ready: true};
    const context = vm.createContext({
        Date: {now: () => now},
        print: () => {},
        midi: {sendShortMsg: (...message) => requests.push(message)},
        engine: {
            setValue(group, control, value) {
                writes.push([group, control, value]);
                values.set(group+'/'+control, value);
            },
            setParameter(group, control, value) {
                writes.push([group, control, value]);
                values.set(group+'/'+control, value);
            },
            getValue(group, control) {
                if (values.has(group+'/'+control)) return values.get(group+'/'+control);
                if (control === 'num_effectslots') {
                    return group === '[EffectRack1_EffectUnit3]' && effectUnits >= 3 ? 3 : 0;
                }
                return control === 'num_samplers' ? 4 : 0;
            },
            makeConnection(group, control, callback) {
                const connection = {callback, isConnected: skin.ready,
                    disconnect() { this.isConnected = false; }};
                connections.push(connection);
                return connection;
            },
            beginTimer(interval, callback) {
                if (!timerAvailable) { return 0; }
                assert.equal(interval, 250);
                const identifier = nextTimer++;
                timers.set(identifier, callback);
                return identifier;
            },
            stopTimer(identifier) { timers.delete(identifier); }
        }
    });
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../TouchFX-scripts.js'), 'utf8'), context);
    if (zed) {
        vm.runInContext(fs.readFileSync(path.join(__dirname, '../integration/zed-extension.js'), 'utf8'), context);
    }
    const mapping = context.TouchFX;
    mapping.init('TouchFX_Virtual', false);
    return {mapping, writes, timers, requests, connections, skin, engine: context.engine, get: control => values.get(mapping.unit+'/'+control),
        advance: milliseconds => { now += milliseconds; mapping.watchdog(); }};
}

test('init reserves unit, disables master/headphone/samplers and enables only deck 1 route', () => {
    const state = fixture();
    assert.equal(state.get('enabled'), 0);
    for (let deck = 1; deck <= 4; deck++) {
        assert.equal(state.get('group_[Channel'+deck+']_enable'), deck === 1 ? 1 : 0);
        assert.equal(state.get('group_[Sampler'+deck+']_enable'), 0);
    }
    assert.equal(state.get('group_[Master]_enable'), 0);
    assert.equal(state.get('group_[Headphone]_enable'), 0);
});

test('axes normalize and route all four MIDI channels without activating', () => {
    const state = fixture();
    for (let channel = 0; channel < 4; channel++) {
        state.mapping.axis(channel, 10, 127, 0xB0+channel);
        state.mapping.axis(channel, 11, 64, 0xB0+channel);
        assert.equal(state.get('super1'), 1);
        assert.equal(state.get('mix'), 64/127);
        assert.equal(state.get('enabled'), 0);
        for (let deck = 1; deck <= 4; deck++) {
            assert.equal(state.get('group_[Channel'+deck+']_enable'), deck === channel+1 ? 1 : 0);
        }
    }
});

test('axis zero and bounds are preserved', () => {
    const state = fixture();
    state.mapping.axis(0, 10, -1, 0xB0);
    state.mapping.axis(0, 11, 500, 0xB0);
    assert.equal(state.get('super1'), 0);
    assert.equal(state.get('mix'), 1);
});

test('note on, explicit note off and velocity-zero note on', () => {
    const state = fixture();
    state.mapping.gate(0, 60, 127, 0x90);
    assert.equal(state.get('enabled'), 1);
    state.mapping.gate(0, 60, 100, 0x80);
    assert.equal(state.get('enabled'), 0);
    state.mapping.gate(0, 60, 127, 0x90);
    state.mapping.gate(0, 60, 0, 0x90);
    assert.equal(state.get('enabled'), 0);
});

test('route changes bypass unit before enabling new route', () => {
    const state = fixture();
    state.mapping.gate(0, 60, 127, 0x90);
    state.writes.length = 0;
    state.mapping.selectDeck(2, 12, 127, 0xB2);
    assert.deepEqual(state.writes[0], [state.mapping.unit, 'enabled', 0]);
    assert.equal(state.get('group_[Channel1]_enable'), 0);
    assert.equal(state.get('group_[Channel3]_enable'), 1);
    assert.equal(state.get('enabled'), 0);
});

test('late note-off from old deck cannot disable new deck', () => {
    const state = fixture();
    state.mapping.gate(2, 60, 127, 0x92);
    state.mapping.gate(0, 60, 0, 0x80);
    assert.equal(state.get('enabled'), 1);
});

test('watchdog disables stuck/latch effect without new MIDI', () => {
    const state = fixture();
    state.mapping.gate(0, 60, 127, 0x90);
    state.advance(1499);
    assert.equal(state.get('enabled'), 1);
    state.advance(1);
    assert.equal(state.get('enabled'), 0);
});

test('heartbeat sustains latch but never reactivates after timeout', () => {
    const state = fixture();
    state.mapping.gate(0, 60, 127, 0x90);
    for (let tick = 0; tick < 20; tick++) {
        state.advance(250);
        state.mapping.heartbeat(0, 119, 127, 0xB0);
    }
    assert.equal(state.get('enabled'), 1);
    state.advance(1500);
    state.mapping.heartbeat(0, 119, 127, 0xB0);
    assert.equal(state.get('enabled'), 0);
    state.mapping.gate(0, 60, 127, 0x90);
    assert.equal(state.get('enabled'), 1);
});

test('zero heartbeat disables and wrong channel does not keep effect alive', () => {
    const state = fixture();
    state.mapping.gate(0, 60, 127, 0x90);
    state.mapping.heartbeat(0, 119, 0, 0xB0);
    assert.equal(state.get('enabled'), 0);
    state.mapping.gate(0, 60, 127, 0x90);
    state.advance(1000);
    state.mapping.heartbeat(3, 119, 127, 0xB3);
    state.advance(500);
    assert.equal(state.get('enabled'), 0);
});

test('invalid channel, note, control and message type are ignored', () => {
    const state = fixture();
    state.writes.length = 0;
    state.mapping.axis(4, 10, 127, 0xB4);
    state.mapping.axis(0, 99, 127, 0xB0);
    state.mapping.gate(0, 61, 127, 0x90);
    state.mapping.gate(0, 60, 127, 0xB0);
    state.mapping.selectDeck(-1, 12, 127, 0xBF);
    assert.equal(state.writes.length, 0);
});

test('missing watchdog fails closed', () => {
    const state = fixture(false);
    state.mapping.gate(0, 60, 127, 0x90);
    assert.equal(state.get('enabled'), 0);
});

test('shutdown disables and clears timer; reinit does not leak timers', () => {
    const state = fixture();
    state.mapping.init('TouchFX_Virtual', false);
    assert.equal(state.timers.size, 1);
    state.mapping.gate(0, 60, 127, 0x90);
    state.mapping.shutdown();
    assert.equal(state.get('enabled'), 0);
    assert.equal(state.timers.size, 0);
});

test('backward wall clock change fails closed', () => {
    const state = fixture();
    state.mapping.gate(0, 60, 127, 0x90);
    state.advance(-100);
    assert.equal(state.get('enabled'), 0);
});

test('ZED uses only unit 3 and preserves D2 unit 1/2', () => {
    const state = fixture(true, true);
    state.mapping.gate(1, 60, 127, 0x91);
    state.mapping.axis(1, 10, 64, 0xB1);
    assert.equal(state.mapping.unit, '[EffectRack1_EffectUnit3]');
    assert.equal(state.get('enabled'), 1);
    assert.ok(state.writes.every(write => write[0] === '[EffectRack1_EffectUnit3]'));
    assert.equal(state.get('group_[Channel3]_enable'), undefined);
});

test('ZED refuses deck 3/4 without changing current effect', () => {
    const state = fixture(true, true);
    state.mapping.gate(0, 60, 127, 0x90);
    const count = state.writes.length;
    state.mapping.gate(2, 60, 127, 0x92);
    state.mapping.axis(3, 11, 127, 0xB3);
    state.mapping.selectDeck(3, 12, 127, 0xB3);
    assert.equal(state.writes.length, count);
    assert.equal(state.mapping.channel, 0);
});

test('ZED button sends open requests only on actual changes, never init', () => {
    const state = fixture(true, true);
    assert.equal(state.requests.length, 0);
    const connection = state.connections.at(-1);
    connection.callback(1);
    connection.callback(1);
    connection.callback(0);
    assert.deepEqual(state.requests, [[0xB0, 120, 127], [0xB0, 120, 127]]);
});

test('ZED without unit 3 fails closed without touching other units', () => {
    const state = fixture(true, true, 2);
    state.mapping.gate(0, 60, 127, 0x90);
    state.mapping.axis(0, 10, 127, 0xB0);
    state.mapping.shutdown();
    assert.equal(state.writes.length, 0);
    assert.equal(state.timers.size, 0);
    assert.equal(state.requests.length, 0);
});

test('ZED shutdown disconnects skin and stale callback cannot open pad', () => {
    const state = fixture(true, true);
    const connection = state.connections.at(-1);
    state.mapping.shutdown();
    assert.equal(connection.isConnected, false);
    connection.callback(1);
    assert.equal(state.requests.length, 0);
    assert.equal(state.timers.size, 0);
});

test('ZED retries late skin binding without opening at startup', () => {
    const state = fixture(true, true);
    state.mapping.shutdown();
    state.skin.ready = false;
    state.mapping.init('TouchFX_Virtual', false);
    assert.equal(state.mapping.skinConnection, null);
    state.skin.ready = true;
    state.advance(250);
    assert.notEqual(state.mapping.skinConnection, null);
    assert.equal(state.requests.filter(message => message[1] === 120).length, 0);
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 0]);
});

test('ZED watchdog still bypasses unit 3 on lost heartbeat', () => {
    const state = fixture(true, true);
    state.mapping.gate(1, 60, 127, 0x91);
    state.advance(1500);
    assert.equal(state.get('enabled'), 0);
    assert.ok(state.writes.every(write => write[0] === '[EffectRack1_EffectUnit3]'));
});

test('ZED callbacks from previous initialization cannot reopen pad', () => {
    const state = fixture(true, true);
    const previous = state.connections.at(-1);
    state.mapping.init('TouchFX_Virtual', false);
    previous.callback(1);
    assert.equal(state.requests.length, 0);
    state.connections.at(-1).callback(1);
    assert.deepEqual(state.requests, [[0xB0, 120, 127]]);
});

test('ZED disconnected skin binding is retried without reopening pad', () => {
    const state = fixture(true, true);
    const previous = state.connections.at(-1);
    previous.isConnected = false;
    state.advance(250);
    assert.notEqual(state.mapping.skinConnection, previous);
    previous.callback(1);
    assert.equal(state.requests.filter(message => message[1] === 120).length, 0);
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 0]);
    state.connections.at(-1).callback(1);
    assert.deepEqual(state.requests.filter(message => message[1] === 120), [[0xB0, 120, 127]]);
});

test('ZED effect selection bypasses before load, resets wet and acknowledges actual slot', () => {
    const state = fixture(true, true);
    const slot = '[EffectRack1_EffectUnit3_Effect1]';
    state.engine.setValue(slot, 'loaded', 1);
    state.mapping.gate(0, 60, 127, 0x90);
    state.writes.length = 0;
    state.mapping.selectEffect(0, 13, 19, 0xB0);
    assert.deepEqual(state.writes[0], [state.mapping.unit, 'enabled', 0]);
    assert.equal(state.get('mix'), 0);
    assert.equal(state.get('super1'), 64/127);
    assert.equal(state.mapping.active, false);
    assert.ok(state.writes.every(write => write[0].startsWith('[EffectRack1_EffectUnit3')));
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 19]);
    assert.deepEqual(state.writes.at(-1), [slot, 'enabled', 1]);
});

test('ZED invalid selector and unsupported decks do not change effects', () => {
    const state = fixture(true, true);
    state.writes.length = 0;
    for (const value of [0, -1, 128, 1.5, NaN]) state.mapping.selectEffect(0, 13, value, 0xB0);
    state.mapping.selectEffect(2, 13, 1, 0xB2);
    state.mapping.selectEffect(0, 12, 1, 0xB0);
    assert.equal(state.writes.length, 0);
});

test('ZED failed load is not enabled or acknowledged as a loaded effect', () => {
    const state = fixture(true, true);
    state.mapping.selectEffect(0, 13, 127, 0xB0);
    assert.equal(state.get('enabled'), 0);
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 0]);
    assert.ok(!state.writes.some(write => write[0].endsWith('Effect1]') && write[1] === 'enabled' && write[2] === 1));
});

test('ZED unavailable unit does not select or report effects', () => {
    const state = fixture(true, true, 2);
    state.mapping.selectEffect(0, 13, 1, 0xB0);
    state.mapping.reportEffect();
    assert.equal(state.writes.length, 0);
    assert.equal(state.requests.length, 0);
});

test('ZED delayed Mixxx effect load eventually enables the slot before acknowledging', () => {
    const state = fixture(true, true);
    const slot = '[EffectRack1_EffectUnit3_Effect1]';
    const setValue = state.engine.setValue;
    setValue(slot, 'loaded_effect', 14);
    setValue(slot, 'loaded', 1);
    setValue(slot, 'enabled', 1);
    const pending = [];
    state.engine.setValue = (group, control, value) => {
        if (control === 'loaded_effect') pending.push([group, control, value]);
        else setValue(group, control, value);
    };
    state.mapping.selectEffect(0, 13, 15, 0xB0);
    for (const args of pending) setValue(...args);
    state.advance(250);
    state.advance(250);
    assert.equal(state.engine.getValue(slot, 'enabled'), 1);
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 15]);
    assert.equal(state.get('enabled'), 0);
});

test('ZED waits for queued slot-enable confirmation and blocks early touch', () => {
    const state = fixture(true, true);
    const slot = '[EffectRack1_EffectUnit3_Effect1]';
    const setValue = state.engine.setValue;
    setValue(slot, 'loaded_effect', 14);
    setValue(slot, 'loaded', 1);
    setValue(slot, 'enabled', 1);
    const pending = [];
    state.engine.setValue = (...args) => pending.push(args);
    state.mapping.selectEffect(0, 13, 15, 0xB0);
    while (pending.length) setValue(...pending.shift());
    state.advance(250);
    state.mapping.gate(0, 60, 127, 0x90);
    assert.equal(state.mapping.active, false);
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 0]);
    while (pending.length) setValue(...pending.shift());
    state.advance(250);
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 15]);
    assert.equal(state.mapping.effectBlocked, false);
});

test('ZED load timeout stays blocked even when an old loaded slot still reports enabled', () => {
    const state = fixture(true, true);
    const slot = '[EffectRack1_EffectUnit3_Effect1]';
    state.engine.setValue(slot, 'loaded_effect', 14);
    state.engine.setValue(slot, 'loaded', 1);
    state.engine.setValue(slot, 'enabled', 1);
    state.engine.setValue = () => {};
    state.mapping.selectEffect(0, 13, 15, 0xB0);
    state.advance(1500);
    state.mapping.gate(0, 60, 127, 0x90);
    assert.equal(state.mapping.active, false);
    assert.equal(state.mapping.effectBlocked, true);
    assert.deepEqual(state.requests.at(-1), [0xB0, 14, 0]);
});

test('ZED repeated deck changes and momentary presses re-enable every time', () => {
    const state = fixture(true, true);
    for (let cycle = 0; cycle < 40; cycle++) {
        const channel = cycle % 2;
        state.mapping.selectDeck(channel, 12, 127, 0xB0+channel);
        state.mapping.axis(channel, 11, 100, 0xB0+channel);
        state.mapping.gate(channel, 60, 127, 0x90+channel);
        assert.equal(state.get('enabled'), 1);
        assert.equal(state.get('group_[Channel'+(channel+1)+']_enable'), 1);
        assert.equal(state.get('group_[Channel'+(2-channel)+']_enable'), 0);
        state.mapping.gate(channel, 60, 0, 0x80+channel);
        assert.equal(state.get('enabled'), 0);
    }
});
