var TouchFXRouter = {};
TouchFX.macroId = 0;
TouchFX.macroBusy = false;
TouchFX.macroRequest = null;
TouchFX.macroPending = null;
TouchFX.lastMacro = null;
TouchFX.amount = 64/127;
TouchFX.macroX = 64/127;
TouchFX.macroY = 0;
TouchFX.lfoStart = 0;
TouchFX.lfoTimer = 0;
TouchFX.macroSlots = {1: 2, 2: 2, 3: 2, 4: 2, 5: 1, 6: 2, 7: 2, 8: 2, 9: 1, 10: 1, 11: 1};

TouchFX.effectSlot = function(index) {
    return TouchFX.unit.replace(']', '_Effect'+index+']');
};

TouchFX.cancelMacro = function() {
    TouchFX.macroId = 0;
    TouchFX.macroBusy = false;
    TouchFX.macroRequest = null;
    TouchFX.macroPending = null;
    TouchFX.lastMacro = null;
};

TouchFX.applyAmount = function() {
    var first = TouchFX.effectSlot(1);
    var second = TouchFX.effectSlot(2);
    if (TouchFX.macroId === 1 || TouchFX.macroId === 6) {
        engine.setValue(second, 'parameter2', (TouchFX.macroId === 6 ? 0.35 : 0.15)+0.4*TouchFX.amount);
    } else if (TouchFX.macroId === 2 || TouchFX.macroId === 3) {
        engine.setValue(TouchFX.macroId === 2 ? first : second, 'parameter4', 0.65*TouchFX.amount);
    } else if (TouchFX.macroId === 4) {
        engine.setValue(first, 'parameter2', 0.707106781+1.792893219*TouchFX.amount);
    } else if (TouchFX.macroId === 5) {
        engine.setValue(first, 'parameter2', 0.15+0.4*TouchFX.amount);
    } else if (TouchFX.macroId === 7) {
        engine.setValue(second, 'parameter1', TouchFX.amount);
    } else if (TouchFX.macroId === 8) {
        engine.setValue(first, 'parameter1', 0.08*TouchFX.amount);
    } else if (TouchFX.macroId === 9) {
        engine.setValue(first, 'parameter4', 0.5*TouchFX.amount);
    }
};

TouchFX.beatSteps = {
    echo: [2, 1, 0.75, 0.5, 0.25, 0.125],
    gate: [4, 2, 1, 0.5, 0.25, 0.125],
    flanger: [16, 8, 4, 2, 1, 0.5]
};
TouchFX.beatStep = function(kind, value) {
    var steps = TouchFX.beatSteps[kind];
    var midiValue = Math.round(Math.max(0, Math.min(1, value))*127);
    return steps[Math.min(steps.length-1, Math.floor(midiValue*steps.length/128))];
};

TouchFX.macroAxis = function(control, value) {
    var first = TouchFX.effectSlot(1);
    var second = TouchFX.effectSlot(2);
    if (control === 10) TouchFX.macroX = value;
    else TouchFX.macroY = value;
    var mode = TouchFX.macroId;
    var horizontal = TouchFX.macroX;
    var vertical = TouchFX.macroY;
    engine.setParameter(TouchFX.unit, 'mix', 1);
    if (mode === 1 || mode === 3 || mode === 4 || mode === 6 || mode === 7 || mode === 10 || mode === 11) {
        engine.setParameter(first, 'meta', horizontal);
    }
    if (mode === 1 || mode === 4 || mode === 6) {
        var beats = TouchFX.beatStep('echo', vertical);
        engine.setValue(second, 'parameter1', beats === 0.125 ? 0 : beats);
    }
    if (mode === 3) engine.setValue(second, 'parameter1', 0.1+0.8*vertical);
    if (mode === 2) {
        engine.setValue(first, 'parameter1', 0.1+0.8*horizontal);
        engine.setParameter(second, 'parameter2', vertical);
    }
    if (mode === 5) engine.setValue(first, 'parameter4', 0.65*vertical);
    if (mode === 7 || mode === 8) {
        engine.setValue(second, 'parameter2', 1/TouchFX.beatStep('gate', mode === 7 ? vertical : horizontal));
        if (mode === 8) engine.setValue(second, 'parameter1', vertical);
    }
    if (mode === 9) {
        engine.setValue(first, 'parameter1', TouchFX.beatStep('flanger', horizontal));
        engine.setParameter(first, 'parameter2', vertical);
    }
    if (mode === 11) engine.setValue(first, 'parameter2', 0.707106781+1.792893219*vertical);
};

TouchFX.macroStart = function() { TouchFX.lfoStart = Date.now(); };
TouchFX.lfoTick = function() {
    if (!TouchFX.active || TouchFX.macroBusy || !TouchFX.validChannel(TouchFX.channel)) return;
    if (TouchFX.macroId !== 5 && TouchFX.macroId !== 10) return;
    var rate = TouchFX.macroId === 10 ? TouchFX.macroY : TouchFX.macroX;
    var phase = (Date.now()-TouchFX.lfoStart)/1000*(0.2+2.8*rate)*2*Math.PI;
    var center = TouchFX.macroId === 10 ? TouchFX.macroX : 0.4;
    var depth = TouchFX.macroId === 10 ? 0.4*TouchFX.amount : 0.2;
    var value = Math.max(0, Math.min(1, center+depth*Math.sin(phase)));
    engine.setParameter(TouchFX.effectSlot(1), TouchFX.macroId === 10 ? 'meta' : 'parameter1', value);
};

TouchFX.loadMacro = function(id, channel, indices) {
    TouchFX.route(channel, false);
    TouchFX.off();
    TouchFX.pendingEffect = null;
    TouchFX.effectBlocked = false;
    TouchFX.macroRequest = null;
    TouchFX.macroBusy = true;
    TouchFX.macroPending = {id: id, indices: indices, started: Date.now(), deadline: Date.now()+2500, enabling: false, clearing: true};
    TouchFX.lastMacro = {id: id, indices: indices.slice()};
    TouchFX.amount = 64/127;
    TouchFX.macroX = 64/127;
    TouchFX.macroY = 0;
    for (var index = 1; index <= engine.getValue(TouchFX.unit, 'num_effectslots'); index++) {
        engine.setValue(TouchFX.effectSlot(index), 'enabled', 0);
    }
    engine.setParameter(TouchFX.unit, 'mix', 0);
    for (var slot = 1; slot <= indices.length; slot++) {
        engine.setValue(TouchFX.effectSlot(slot), 'clear', 1);
        engine.setValue(TouchFX.effectSlot(slot), 'clear', 0);
    }
};

TouchFX.macroMessage = function(channel, control, value, status, group) {
    if (!TouchFX.validChannel(channel) || Math.floor(value) !== value || value < 0 || value > 127) return;
    if (control === 24) {
        if (channel !== TouchFX.channel || TouchFX.macroBusy) return;
        TouchFX.amount = value/127;
        TouchFX.applyAmount();
        return;
    }
    if (control === 26 && value === 127) {
        if (channel !== TouchFX.channel) return;
        TouchFX.off();
        if (TouchFX.lastMacro) TouchFX.loadMacro(TouchFX.lastMacro.id, channel, TouchFX.lastMacro.indices);
        else if (TouchFX.selectEffect) {
            var current = engine.getValue(TouchFX.effectSlot(1), 'loaded_effect');
            if (current > 0) TouchFX.selectEffect(channel, 13, current, status, group);
        }
        return;
    }
    if (control === 16) {
        var count = TouchFX.macroSlots[value];
        if (!count || engine.getValue(TouchFX.unit, 'num_effectslots') < count) return;
        if ((value === 5 || value === 10) && !TouchFX.lfoTimer) {
            TouchFX.failMacro();
            return;
        }
        TouchFX.route(channel, false);
        TouchFX.off();
        TouchFX.pendingEffect = null;
        TouchFX.effectBlocked = false;
        TouchFX.macroPending = null;
        TouchFX.macroBusy = true;
        TouchFX.macroRequest = {id: value, channel: channel, first: null, second: null,
            started: Date.now(), deadline: Date.now()+1500};
        return;
    }
    var request = TouchFX.macroRequest;
    if (!request || request.channel !== channel || Date.now() >= request.deadline || Date.now() < request.started) return;
    if (control === 20 || control === 21) request[control === 20 ? 'first' : 'second'] = value;
    if (control === 22 && value === request.id && request.first > 0 && request.second !== null) {
        var slots = TouchFX.macroSlots[request.id];
        if (slots === 2 && (request.second <= 0 || request.first === request.second)) return;
        if (slots === 1 && request.second !== 0) return;
        TouchFX.loadMacro(request.id, channel, slots === 2 ? [request.first, request.second] : [request.first]);
    }
};

TouchFX.failMacro = function() {
    TouchFX.macroRequest = null;
    TouchFX.macroPending = null;
    TouchFX.macroId = 0;
    TouchFX.macroBusy = true;
    TouchFX.off();
    for (var index = 1; index <= engine.getValue(TouchFX.unit, 'num_effectslots'); index++) {
        engine.setValue(TouchFX.effectSlot(index), 'enabled', 0);
    }
};

TouchFX.macroWatchdog = function() {
    if (!TouchFX.validChannel(TouchFX.channel)) return;
    var pending = TouchFX.macroPending;
    var transaction = pending || TouchFX.macroRequest;
    if (transaction && (Date.now() >= transaction.deadline || Date.now() < transaction.started)) {
        TouchFX.failMacro();
        pending = null;
    }
    if (pending) {
        if (pending.clearing && pending.indices.every(function(value, index) {
            return engine.getValue(TouchFX.effectSlot(index+1), 'loaded') === 0;
        })) {
            pending.clearing = false;
            for (var slot = 1; slot <= pending.indices.length; slot++) {
                engine.setValue(TouchFX.effectSlot(slot), 'loaded_effect', pending.indices[slot-1]);
            }
        }
        var loaded = pending.indices.every(function(value, index) {
            return engine.getValue(TouchFX.effectSlot(index+1), 'loaded_effect') === value &&
                engine.getValue(TouchFX.effectSlot(index+1), 'loaded') > 0;
        });
        if (loaded && !pending.clearing && !pending.enabling) {
            TouchFX.macroId = pending.id;
            var first = TouchFX.effectSlot(1);
            var second = TouchFX.effectSlot(2);
            if (pending.id === 2) { engine.setValue(second, 'parameter1', 8); engine.setValue(second, 'parameter5', 0.5); }
            if (pending.id === 1 || pending.id === 4 || pending.id === 6) {
                engine.setValue(second, 'button_parameter1', 1);
                engine.setValue(second, 'button_parameter2', 0);
                if (pending.id !== 4) engine.setValue(second, 'parameter4', pending.id === 6 ? 0.5 : 0.35);
            }
            if (pending.id === 5) {
                engine.setValue(first, 'parameter2', 0.4);
                engine.setValue(first, 'parameter4', 0.5);
                engine.setValue(first, 'button_parameter1', 0);
            }
            if (pending.id === 7 || pending.id === 8) {
                engine.setValue(second, 'button_parameter1', 1);
                engine.setValue(second, 'button_parameter2', 0);
            }
            if (pending.id === 2 || pending.id === 9) engine.setValue(pending.id === 2 ? second : first, 'button_parameter1', 0);
            if (pending.id === 9) engine.setValue(first, 'parameter5', 0.5);
            TouchFX.applyAmount();
            TouchFX.macroAxis(10, 64/127);
            engine.setParameter(TouchFX.unit, 'mix', 0);
            for (var index = 1; index <= pending.indices.length; index++) engine.setValue(TouchFX.effectSlot(index), 'enabled', 1);
            pending.enabling = true;
        }
        if (loaded && !pending.clearing && pending.enabling && pending.indices.every(function(value, index) {
            return engine.getValue(TouchFX.effectSlot(index+1), 'enabled') > 0;
        })) { TouchFX.macroBusy = false; TouchFX.macroPending = null; }
    }
    var mask = 0;
    for (var index = 1; index <= 2; index++) {
        if (engine.getValue(TouchFX.effectSlot(index), 'loaded') && engine.getValue(TouchFX.effectSlot(index), 'enabled')) mask |= 1 << (index-1);
    }
    midi.sendShortMsg(0xB0, 18, TouchFX.macroBusy ? 127 : TouchFX.macroId);
    midi.sendShortMsg(0xB0, 19, mask & 2 ? engine.getValue(TouchFX.effectSlot(2), 'loaded_effect') : 0);
    midi.sendShortMsg(0xB0, 23, mask);
    if (!TouchFX.reportEffect) {
        midi.sendShortMsg(0xB0, 14, !TouchFX.macroBusy && (mask & 1) ? engine.getValue(TouchFX.effectSlot(1), 'loaded_effect') : 0);
        midi.sendShortMsg(0xB0, 15, (engine.getValue(TouchFX.unit, 'enabled') > 0 ? 1 : 0) | (mask & 1 ? 18 : 0));
    }
};

TouchFXRouter.init = function() {
    if (TouchFX.validChannel(TouchFX.channel)) {
        TouchFX.off();
        for (var index = 1; index <= engine.getValue(TouchFX.unit, 'num_effectslots'); index++) {
            engine.setValue(TouchFX.effectSlot(index), 'enabled', 0);
        }
    }
    if (TouchFX.lfoTimer) engine.stopTimer(TouchFX.lfoTimer);
    TouchFX.lfoTimer = engine.beginTimer(50, TouchFX.lfoTick);
};
TouchFXRouter.shutdown = function() {
    if (TouchFX.lfoTimer) engine.stopTimer(TouchFX.lfoTimer);
    TouchFX.lfoTimer = 0;
    TouchFX.cancelMacro();
};
