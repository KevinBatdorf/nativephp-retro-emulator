<?php

use KevinBatdorf\RetroEmulator\Components\Emulator as EmulatorComponent;
use KevinBatdorf\RetroEmulator\Elements\Emulator as EmulatorElement;
use KevinBatdorf\RetroEmulator\Emulator;
use KevinBatdorf\RetroEmulator\Events\EmulatorError;
use KevinBatdorf\RetroEmulator\Events\EmulatorPaused;
use KevinBatdorf\RetroEmulator\Events\EmulatorResumed;
use KevinBatdorf\RetroEmulator\Events\EmulatorStarted;
use KevinBatdorf\RetroEmulator\Events\EmulatorStopped;
use KevinBatdorf\RetroEmulator\Events\MemoryChanged;
use KevinBatdorf\RetroEmulator\Events\MemoryRead;
use KevinBatdorf\RetroEmulator\RetroEmulatorServiceProvider;

// ---------------------------------------------------------------------------
// nativephp.json manifest
// ---------------------------------------------------------------------------

describe('Plugin Manifest', function () {
    beforeEach(function () {
        $this->manifest = json_decode(
            file_get_contents(dirname(__DIR__).'/nativephp.json'),
            true,
        );
    });

    it('is valid JSON', function () {
        expect($this->manifest)->toBeArray();
    });

    it('has the correct namespace', function () {
        expect($this->manifest['namespace'])->toBe('Emulator');
    });

    it('declares all bridge functions', function () {
        $names = array_column($this->manifest['bridge_functions'], 'name');

        $expected = [
            'Emulator.Boot',
            'Emulator.LoadSystem',
            'Emulator.LoadRom',
            'Emulator.Pause',
            'Emulator.Resume',
            'Emulator.Stop',
            'Emulator.StateSave',
            'Emulator.StateLoad',
            'Emulator.UndoStateSave',
            'Emulator.UndoStateLoad',
            'Emulator.ReadMemory',
            'Emulator.ReadMemoryAsync',
            'Emulator.WriteMemory',
            'Emulator.WatchMemory',
            'Emulator.UnwatchMemory',
            'Emulator.ClearMemoryWatches',
            'Emulator.SetAudio',
            'Emulator.SetVideo',
            'Emulator.Configure',
            'Emulator.SetSystemOptions',
            'Emulator.FastForward',
            'Emulator.SetInputMapping',
            'Emulator.SetRumble',
            'Emulator.SetShader',
            'Emulator.AddCheat',
            'Emulator.RemoveCheat',
            'Emulator.ClearCheats',
            'Emulator.PressButton',
            'Emulator.ReleaseButton',
            'Emulator.SetButtons',
            'Emulator.Screenshot',
            'Emulator.GetStatus',
            'Emulator.GetRegion',
            'Emulator.GetPorts',
            'Emulator.GetSystems',
        ];

        foreach ($expected as $name) {
            expect($names)->toContain($name);
        }
    });

    it('has android and ios entries for every bridge function', function () {
        foreach ($this->manifest['bridge_functions'] as $fn) {
            expect($fn)->toHaveKeys(['name', 'android', 'ios', 'description']);
            expect($fn['android'])->toStartWith('com.kevinbatdorf.plugins.retroemulator.EmulatorFunctions.');
            expect($fn['ios'])->toStartWith('EmulatorFunctions.');
        }
    });

    it('declares all events', function () {
        $events = $this->manifest['events'];

        $expected = [
            'KevinBatdorf\\RetroEmulator\\Events\\EmulatorStarted',
            'KevinBatdorf\\RetroEmulator\\Events\\EmulatorStopped',
            'KevinBatdorf\\RetroEmulator\\Events\\EmulatorPaused',
            'KevinBatdorf\\RetroEmulator\\Events\\EmulatorResumed',
            'KevinBatdorf\\RetroEmulator\\Events\\MemoryRead',
            'KevinBatdorf\\RetroEmulator\\Events\\MemoryChanged',
            'KevinBatdorf\\RetroEmulator\\Events\\EmulatorError',
        ];

        foreach ($expected as $event) {
            expect($events)->toContain($event);
        }
    });

    it('registers the correct service provider', function () {
        expect($this->manifest['service_provider'])
            ->toBe('KevinBatdorf\\RetroEmulator\\RetroEmulatorServiceProvider');
    });

    it('declares the emulator component', function () {
        $components = $this->manifest['components'] ?? [];
        $types = array_column($components, 'type');

        expect($types)->toContain('emulator');

        $emulator = current(array_filter($components, fn ($c) => $c['type'] === 'emulator'));
        expect($emulator['element'])->toBe('KevinBatdorf\\RetroEmulator\\Elements\\Emulator');
        expect($emulator['blade'])->toBe('KevinBatdorf\\RetroEmulator\\Components\\Emulator');
        expect($emulator['android_renderer'])->toBe('com.kevinbatdorf.plugins.retroemulator.EmulatorRenderer');
        expect($emulator['ios_renderer'])->toBe('EmulatorRenderer');
    });
});

// ---------------------------------------------------------------------------
// Service provider
// ---------------------------------------------------------------------------

describe('Service provider', function () {
    it('class exists', function () {
        expect(class_exists(RetroEmulatorServiceProvider::class))->toBeTrue();
    });
});

// ---------------------------------------------------------------------------
// Emulator class
// ---------------------------------------------------------------------------

describe('Emulator class', function () {
    it('exists', function () {
        expect(class_exists(Emulator::class))->toBeTrue();
    });

    it('getSystems is static', function () {
        $ref = new ReflectionMethod(Emulator::class, 'getSystems');
        expect($ref->isStatic())->toBeTrue();
    });

    it('has all instance methods', function () {
        $methods = [
            'boot', 'loadSystem', 'loadRom',
            'pause', 'resume', 'stop',
            'stateSave', 'stateLoad', 'undoStateSave', 'undoStateLoad',
            'readMemory', 'readMemoryAsync', 'writeMemory',
            'watchMemory', 'unwatchMemory', 'clearMemoryWatches',
            'setAudio', 'setVideo', 'configure', 'setSystemOptions',
            'fastForward',
            'setInputMapping', 'setRumble',
            'setShader',
            'addCheat', 'removeCheat', 'clearCheats',
            'pressButton', 'releaseButton', 'setButtons',
            'screenshot', 'getStatus', 'getPorts', 'getRegion',
        ];

        foreach ($methods as $method) {
            expect(method_exists(Emulator::class, $method))
                ->toBeTrue("Emulator::{$method}() is missing");
        }
    });

    it('boot returns an Emulator instance', function () {
        expect((new Emulator)->boot('main'))->toBeInstanceOf(Emulator::class);
    });

    it('getStatus returns stopped without native runtime', function () {
        expect((new Emulator)->getStatus())->toBe('stopped');
    });

    it('readMemory returns empty array without native runtime', function () {
        expect((new Emulator)->readMemory(0x7E0010))->toBe([]);
    });

    it('getSystems returns empty array without native runtime', function () {
        expect(Emulator::getSystems())->toBe([]);
    });
});

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

describe('Events', function () {
    it('EmulatorStarted has correct properties', function () {
        $event = new EmulatorStarted('main', 'sfc', '/path/to/rom.sfc');
        expect($event->surface)->toBe('main');
        expect($event->system)->toBe('sfc');
        expect($event->romPath)->toBe('/path/to/rom.sfc');
    });

    it('EmulatorStopped has correct properties', function () {
        $event = new EmulatorStopped('main');
        expect($event->surface)->toBe('main');
    });

    it('EmulatorPaused has correct properties', function () {
        $event = new EmulatorPaused('main');
        expect($event->surface)->toBe('main');
    });

    it('EmulatorResumed has correct properties', function () {
        $event = new EmulatorResumed('main');
        expect($event->surface)->toBe('main');
    });

    it('MemoryRead has correct properties', function () {
        $event = new MemoryRead('main', 0x7E0010, [0x07]);
        expect($event->surface)->toBe('main');
        expect($event->address)->toBe(0x7E0010);
        expect($event->bytes)->toBe([0x07]);
    });

    it('MemoryChanged has correct properties', function () {
        $event = new MemoryChanged('main', 0x7E0010, 0x07, 0x09);
        expect($event->surface)->toBe('main');
        expect($event->address)->toBe(0x7E0010);
        expect($event->oldValue)->toBe(0x07);
        expect($event->newValue)->toBe(0x09);
    });

    it('EmulatorError has correct properties', function () {
        $event = new EmulatorError('main', 'CRASH', 'Segmentation fault in ares core');
        expect($event->surface)->toBe('main');
        expect($event->code)->toBe('CRASH');
        expect($event->message)->toBe('Segmentation fault in ares core');
    });
});

// ---------------------------------------------------------------------------
// Component registry classes
// ---------------------------------------------------------------------------

describe('Component registry', function () {
    it('EmulatorElement exists with the emulator type', function () {
        expect(class_exists(EmulatorElement::class))->toBeTrue();

        // Full EDGE contract coverage lives in EdgeElementTest.php.
        expect((new EmulatorElement)->getType())->toBe('emulator');
    });

    it('EmulatorComponent exists', function () {
        expect(class_exists(EmulatorComponent::class))->toBeTrue();
    });
});

// ---------------------------------------------------------------------------
// Composer config
// ---------------------------------------------------------------------------

describe('Composer config', function () {
    beforeEach(function () {
        $this->composer = json_decode(
            file_get_contents(dirname(__DIR__).'/composer.json'),
            true,
        );
    });

    it('is type nativephp-plugin', function () {
        expect($this->composer['type'])->toBe('nativephp-plugin');
    });

    it('points to nativephp.json manifest', function () {
        expect($this->composer['extra']['nativephp']['manifest'])->toBe('nativephp.json');
    });

    it('autoloads the correct namespace', function () {
        expect($this->composer['autoload']['psr-4'])
            ->toHaveKey('KevinBatdorf\\RetroEmulator\\');
    });

    it('registers the service provider', function () {
        expect($this->composer['extra']['laravel']['providers'])
            ->toContain('KevinBatdorf\\RetroEmulator\\RetroEmulatorServiceProvider');
    });
});

// ---------------------------------------------------------------------------
// Phase 7 — bridge response parsing (native layer mocked via nativephp_call stub)
// ---------------------------------------------------------------------------

describe('Bridge response parsing', function () {
    afterEach(function () {
        $GLOBALS['__nativephp_mock'] = [];
    });

    it('getStatus parses running from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{"status":"running"}';
        expect((new Emulator)->getStatus())->toBe('running');
    });

    it('getStatus parses paused from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{"status":"paused"}';
        expect((new Emulator)->getStatus())->toBe('paused');
    });

    it('getStatus returns stopped when native returns no status key', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{}';
        expect((new Emulator)->getStatus())->toBe('stopped');
    });

    it('readMemory parses bytes from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ReadMemory'] = '{"address":8257552,"bytes":[7,0]}';
        expect((new Emulator)->readMemory(0x7E0010, 2))->toBe([7, 0]);
    });

    it('readMemory returns empty array when native returns no bytes key', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ReadMemory'] = '{}';
        expect((new Emulator)->readMemory(0x7E0010))->toBe([]);
    });

    it('getRegion parses region from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetRegion'] = '{"region":"NTSC"}';
        expect((new Emulator)->getRegion())->toBe('NTSC');
    });

    it('getRegion returns empty string when native returns no region key', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetRegion'] = '{}';
        expect((new Emulator)->getRegion())->toBe('');
    });

    it('getPorts parses ports array from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetPorts'] = json_encode([
            'ports' => [
                ['port' => 1, 'buttons' => ['B', 'Y', 'Select', 'Start', 'Up', 'Down', 'Left', 'Right', 'A', 'X', 'L', 'R']],
            ],
        ]);
        $ports = (new Emulator)->getPorts();
        expect($ports)->toHaveCount(1);
        expect($ports[0]['port'])->toBe(1);
        expect($ports[0]['buttons'])->toContain('B');
        expect($ports[0]['buttons'])->toContain('Start');
    });

    it('getSystems parses systems array from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetSystems'] = json_encode([
            'systems' => [
                ['id' => 'sfc', 'name' => 'SNES / Super Famicom', 'biosRequired' => false, 'stable' => true],
                ['id' => 'ps1', 'name' => 'PlayStation', 'biosRequired' => true, 'stable' => false],
            ],
        ]);
        $systems = Emulator::getSystems();
        expect($systems)->toHaveCount(2);
        expect($systems[0]['id'])->toBe('sfc');
        expect($systems[0]['biosRequired'])->toBeFalse();
        expect($systems[1]['id'])->toBe('ps1');
        expect($systems[1]['biosRequired'])->toBeTrue();
        expect($systems[1]['stable'])->toBeFalse();
    });

    it('boot returns Emulator instance after successful native call', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Boot'] = '{"status":"bound","surface":"main"}';
        $emu = (new Emulator)->boot('main');
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('loadRom returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.LoadRom'] = '{"status":"loading","path":"/path/to/rom.sfc"}';
        $emu = (new Emulator)->loadRom('/path/to/rom.sfc');
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('pause returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Pause'] = '{"status":"paused"}';
        $emu = (new Emulator)->pause();
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('watchMemory returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.WatchMemory'] = '{"status":"watching","count":2}';
        $emu = (new Emulator)->watchMemory([0x7E0010, ['address' => 0x7EF340, 'length' => 2]]);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('fastForward returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.FastForward'] = '{"status":"fast"}';
        $emu = (new Emulator)->fastForward(true);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('stateSave returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.StateSave'] = '{"status":"saved","slot":1}';
        $emu = (new Emulator)->stateSave(1);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('stateLoad returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.StateLoad'] = '{"status":"loaded","slot":1}';
        $emu = (new Emulator)->stateLoad(1);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });
});
