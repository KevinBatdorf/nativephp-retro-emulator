<?php

use Illuminate\Container\Container;
use KevinBatdorf\RetroEmulator\Accuracy;
use KevinBatdorf\RetroEmulator\AspectCorrection;
use KevinBatdorf\RetroEmulator\Backend;
use KevinBatdorf\RetroEmulator\Buttons\FcButton;
use KevinBatdorf\RetroEmulator\Buttons\GbaButton;
use KevinBatdorf\RetroEmulator\Buttons\GbButton;
use KevinBatdorf\RetroEmulator\Buttons\MdButton;
use KevinBatdorf\RetroEmulator\Buttons\SfcButton;
use KevinBatdorf\RetroEmulator\Commands\CopyAssetsCommand;
use KevinBatdorf\RetroEmulator\Components\Emulator as EmulatorComponent;
use KevinBatdorf\RetroEmulator\Config\Config;
use KevinBatdorf\RetroEmulator\Config\FcConfig;
use KevinBatdorf\RetroEmulator\Config\GbaConfig;
use KevinBatdorf\RetroEmulator\Config\GbConfig;
use KevinBatdorf\RetroEmulator\Config\MdConfig;
use KevinBatdorf\RetroEmulator\Config\RegionalSystemConfig;
use KevinBatdorf\RetroEmulator\Config\SfcConfig;
use KevinBatdorf\RetroEmulator\Controller;
use KevinBatdorf\RetroEmulator\Device;
use KevinBatdorf\RetroEmulator\Elements\Emulator as EmulatorElement;
use KevinBatdorf\RetroEmulator\Emulator;
use KevinBatdorf\RetroEmulator\EmulatorErrorCode;
use KevinBatdorf\RetroEmulator\EmulatorException;
use KevinBatdorf\RetroEmulator\Events\EmulatorError;
use KevinBatdorf\RetroEmulator\Events\EmulatorPaused;
use KevinBatdorf\RetroEmulator\Events\EmulatorResumed;
use KevinBatdorf\RetroEmulator\Events\EmulatorStarted;
use KevinBatdorf\RetroEmulator\Events\EmulatorStopped;
use KevinBatdorf\RetroEmulator\Events\MemoryChanged;
use KevinBatdorf\RetroEmulator\Events\MemoryRead;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\Region;
use KevinBatdorf\RetroEmulator\RetroEmulatorServiceProvider;
use KevinBatdorf\RetroEmulator\Status;
use KevinBatdorf\RetroEmulator\System;
use KevinBatdorf\RetroEmulator\VideoOutput;
use Psr\Log\AbstractLogger;

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
            'Emulator.StageSlot',
            'Emulator.ReadMemory',
            'Emulator.ReadMemoryAsync',
            'Emulator.WriteMemory',
            'Emulator.WatchMemory',
            'Emulator.UnwatchMemory',
            'Emulator.ClearMemoryWatches',
            'Emulator.SetAudio',
            'Emulator.SetVideo',
            'Emulator.Configure',
            'Emulator.PickRom',
            'Emulator.Rewind',
            'Emulator.ToggleRewind',
            'Emulator.SetSystemOptions',
            'Emulator.FastForward',
            'Emulator.SetInputMapping',
            'Emulator.ConnectDevice',
            'Emulator.SetAxis',
            'Emulator.AimAt',
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
            'Emulator.GetInputDevices',
            'Emulator.GetPressedButtons',
            'Emulator.WindowMetrics',
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
            'KevinBatdorf\\RetroEmulator\\Events\\RomPicked',
            'KevinBatdorf\\RetroEmulator\\Events\\WindowMetricsChanged',
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
        expect($emulator['android_renderer'])->toBe('com.kevinbatdorf.plugins.retroemulator.EmulatorSurface');
        expect($emulator['ios_renderer'])->toBe('EmulatorSurfaceView');
    });

    it('declares the dpad component', function () {
        $components = $this->manifest['components'] ?? [];

        expect(array_column($components, 'type'))->toContain('dpad');

        $dpad = current(array_filter($components, fn ($c) => $c['type'] === 'dpad'));
        expect($dpad['element'])->toBe('KevinBatdorf\\RetroEmulator\\Elements\\Dpad');
        expect($dpad['blade'])->toBe('KevinBatdorf\\RetroEmulator\\Components\\Dpad');
        expect($dpad['android_renderer'])->toBe('com.kevinbatdorf.plugins.retroemulator.DpadSurface');
        expect($dpad['ios_renderer'])->toBe('DpadSurfaceView');
    });

    it('ships a renderer for every declared component on both platforms', function () {
        foreach ($this->manifest['components'] ?? [] as $component) {
            $android = str($component['android_renderer'])->afterLast('.')->toString();

            expect(file_exists(__DIR__."/../resources/android/src/{$android}.kt"))->toBeTrue();
            expect(file_exists(__DIR__."/../resources/ios/Sources/{$component['ios_renderer']}.swift"))->toBeTrue();
        }
    });
});

describe('Service provider', function () {
    it('class exists', function () {
        expect(class_exists(RetroEmulatorServiceProvider::class))->toBeTrue();
    });
});

describe('Emulator class', function () {
    it('exists', function () {
        expect(class_exists(Emulator::class))->toBeTrue();
    });

    it('systems is static', function () {
        $ref = new ReflectionMethod(Emulator::class, 'systems');
        expect($ref->isStatic())->toBeTrue();
    });

    it('has all instance methods', function () {
        $methods = [
            'surface', 'loadSystem', 'loadRom',
            'pause', 'resume', 'stop',
            'saveState', 'loadState', 'undoSaveState', 'undoLoadState',
            'readMemory', 'readMemoryAsync', 'writeMemory',
            'watchMemory', 'unwatchMemory', 'clearMemoryWatches',
            'setVolume', 'setBalance', 'setVideo', 'configure', 'setSystemOptions',
            'fastForward', 'toggleRewind', 'setSpeed',
            'connectDevice', 'connectMultitap', 'getDevice', 'setRumble',
            'setShader',
            'addCheat', 'removeCheat', 'clearCheats',
            'screenshot', 'status', 'ports', 'region',
        ];

        foreach ($methods as $method) {
            expect(method_exists(Emulator::class, $method))
                ->toBeTrue("Emulator::{$method}() is missing");
        }
    });

    it('Controller has all input methods', function () {
        foreach (['press', 'release', 'setButtons', 'setAxis', 'aimAt', 'remap'] as $method) {
            expect(method_exists(Controller::class, $method))
                ->toBeTrue("Controller::{$method}() is missing");
        }
    });

    it('surface returns an Emulator instance', function () {
        expect(Emulator::surface('main'))->toBeInstanceOf(Emulator::class);
    });

    it('status returns Stopped without native runtime', function () {
        expect(Emulator::surface()->status())->toBe(Status::Stopped);
    });

    it('readMemory returns empty array without native runtime', function () {
        expect(Emulator::surface()->readMemory(0x7E0010))->toBe([]);
    });

    it('systems returns empty array without native runtime', function () {
        expect(Emulator::systems())->toBe([]);
    });

    it('capabilities returns empty array without native runtime', function () {
        expect(Emulator::capabilities('gb', 'ares'))->toBe([]);
    });
});

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

    it('EmulatorError has correct properties, coercing the wire code to the enum', function () {
        $event = new EmulatorError('main', 'LOAD_FAILED', 'boot failed; emulator stopped');
        expect($event->surface)->toBe('main');
        expect($event->code)->toBe(EmulatorErrorCode::LoadFailed);
        expect($event->message)->toBe('boot failed; emulator stopped');
    });

    it('every event exposes the standard dispatch helper, like the SDK events', function () {
        $events = [
            EmulatorStarted::class, EmulatorStopped::class, EmulatorPaused::class,
            EmulatorResumed::class, MemoryRead::class, MemoryChanged::class,
            EmulatorError::class,
        ];

        foreach ($events as $event) {
            expect(method_exists($event, 'dispatch'))->toBeTrue($event);
        }
    });
});

describe('Error handling', function () {
    afterEach(function () {
        $GLOBALS['__nativephp_mock'] = [];
    });

    it('error codes match the native source', function () {
        $sources = file_get_contents(dirname(__DIR__).'/resources/android/src/EmulatorFunctions.kt')
            .file_get_contents(dirname(__DIR__).'/resources/android/src/EmulatorRenderer.kt')
            .file_get_contents(dirname(__DIR__).'/resources/ios/Sources/EmulatorFunctions.swift')
            .file_get_contents(dirname(__DIR__).'/resources/ios/Sources/EmulatorRenderer.swift')
            .file_get_contents(dirname(__DIR__).'/android/app/src/main/cpp/emulator_jni.cpp')
            .file_get_contents(dirname(__DIR__).'/ios/emulator_api.cpp')
            .file_get_contents(dirname(__DIR__).'/native/host/emulator_host.cpp');

        // The enum is the union across platforms; input-validation codes
        // like UNKNOWN_BUTTON originate in the shared host.
        preg_match_all(
            '/(?:BridgeResponse\.error|\.onError)\s*\(\s*(?:code:\s*)?"([A-Z_]+)"'
            .'|operationalError\([^,]+,\s*(?:code:\s*)?"([A-Z_]+)"'
            .'|(?:statusRet|\bret)\s*\(\s*\(?\s*(?:std::string\s*\(\s*)?"([A-Z_]+)[":]'
            // Bare returns require an underscore so region names ("NTSC")
            // in the host's resolver don't read as codes.
            .'|return\s+(?:env->NewStringUTF\s*\(\s*)?"([A-Z]+_[A-Z_]+)[":]/s',
            $sources,
            $m,
        );
        $native = array_values(array_unique(array_filter(array_merge($m[1], $m[2], $m[3], $m[4]))));
        $enum = array_map(fn ($case) => $case->value, EmulatorErrorCode::cases());

        sort($native);
        sort($enum);
        expect($enum)->toBe($native);
    });

    it('throws EmulatorException carrying the code on a programmer error', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ReadMemory'] =
            '{"status":"error","code":"READ_FAILED","message":"out of range"}';

        try {
            Emulator::surface('main')->readMemory(0xFFFFFF);
            throw new Exception('expected EmulatorException, none thrown');
        } catch (EmulatorException $e) {
            expect($e->errorCode)->toBe(EmulatorErrorCode::ReadFailed);
            expect($e->getMessage())->toBe('out of range');
        }
    });

    it('throws on a fluent command that a programmer got wrong', function () {
        $GLOBALS['__nativephp_mock']['Emulator.LoadSystem'] =
            '{"status":"error","code":"UNSUPPORTED_SYSTEM","message":"nope"}';

        expect(fn () => Emulator::surface('main')->loadSystem('xyz'))
            ->toThrow(EmulatorException::class);
    });

    it('throws when a remap names an unknown button', function () {
        $GLOBALS['__nativephp_mock']['Emulator.SetInputMapping'] =
            '{"status":"error","code":"UNKNOWN_BUTTON","message":"Unknown button: q"}';

        expect(fn () => Emulator::surface('main')->getDevice(1)->remap(['q' => 'a']))
            ->toThrow(EmulatorException::class);
    });

    it('throws when connecting an unsupported device', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ConnectDevice'] =
            '{"status":"error","code":"UNSUPPORTED_DEVICE","message":"device not supported: Mouse"}';

        expect(fn () => Emulator::surface('main')->connectDevice(1, Device::Mouse))
            ->toThrow(EmulatorException::class);
    });

    it('does not throw on SCREENSHOT_FAILED — screenshot() returns null', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Screenshot'] =
            '{"status":"error","code":"SCREENSHOT_FAILED","message":"no frame"}';

        expect(Emulator::surface('main')->screenshot())->toBeNull();
    });

    it('never throws an operational code, even as a bridge error', function () {
        // Operational outcomes travel the event channel. A platform still
        // returning one as a bridge error must not reach the dev as a throw.
        $GLOBALS['__nativephp_mock']['Emulator.LoadRom'] =
            '{"status":"error","code":"ROM_NOT_FOUND","message":"missing"}';

        expect(fn () => Emulator::surface('main')->loadRom('/nope.sfc'))
            ->not->toThrow(EmulatorException::class);
    });

    it('a successful response never throws', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Pause'] = '{"status":"paused"}';

        expect(fn () => Emulator::surface('main')->pause())
            ->not->toThrow(EmulatorException::class);
    });
});

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

describe('Bridge response parsing', function () {
    afterEach(function () {
        $GLOBALS['__nativephp_mock'] = [];
    });

    it('status parses running from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{"status":"running"}';
        expect(Emulator::surface()->status())->toBe(Status::Running);
    });

    it('status parses paused from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{"status":"paused"}';
        expect(Emulator::surface()->status())->toBe(Status::Paused);
    });

    it('status returns Stopped when native returns no status key', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{}';
        expect(Emulator::surface()->status())->toBe(Status::Stopped);
    });

    it('backend reports the engine actually serving, empty before staging', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{"status":"running","backend":"ares"}';
        expect(Emulator::surface()->backend())->toBe('ares');

        $GLOBALS['__nativephp_mock']['Emulator.GetStatus'] = '{"status":"stopped"}';
        expect(Emulator::surface()->backend())->toBe('');
    });

    it('readMemory parses bytes from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ReadMemory'] = '{"address":8257552,"bytes":[7,0]}';
        expect((new Emulator)->readMemory(0x7E0010, 2))->toBe([7, 0]);
    });

    it('readMemory returns empty array when native returns no bytes key', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ReadMemory'] = '{}';
        expect((new Emulator)->readMemory(0x7E0010))->toBe([]);
    });

    it('region parses region from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetRegion'] = '{"region":"NTSC"}';
        expect(Emulator::surface()->region())->toBe('NTSC');
    });

    it('region returns empty string when native returns no region key', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetRegion'] = '{}';
        expect(Emulator::surface()->region())->toBe('');
    });

    it('ports parses ports array from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetPorts'] = json_encode([
            'ports' => [
                ['port' => 1, 'buttons' => ['B', 'Y', 'Select', 'Start', 'Up', 'Down', 'Left', 'Right', 'A', 'X', 'L', 'R']],
            ],
        ]);
        $ports = Emulator::surface()->ports();
        expect($ports)->toHaveCount(1);
        expect($ports[0]['port'])->toBe(1);
        expect($ports[0]['buttons'])->toContain('B');
        expect($ports[0]['buttons'])->toContain('Start');
    });

    it('systems parses systems array from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetSystems'] = json_encode([
            'systems' => [
                ['id' => 'sfc', 'name' => 'SNES / Super Famicom', 'stable' => true, 'supported' => true],
                ['id' => 'gba', 'name' => 'Game Boy Advance', 'stable' => true, 'supported' => true],
            ],
        ]);
        $systems = Emulator::systems();
        expect($systems)->toHaveCount(2);
        expect($systems[0]['id'])->toBe('sfc');
        expect($systems[1]['id'])->toBe('gba');
        expect($systems[1]['supported'])->toBeTrue();
    });

    it('inputDevices parses controller names from native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetInputDevices'] = json_encode([
            'devices' => ['Retroid Pocket Gamepad', 'DualSense Wireless Controller'],
        ]);
        $devices = Emulator::inputDevices();
        expect($devices)->toBe(['Retroid Pocket Gamepad', 'DualSense Wireless Controller']);
    });

    it('inputDevices returns empty array when none connected', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetInputDevices'] = json_encode(['devices' => []]);
        expect(Emulator::inputDevices())->toBe([]);
    });

    it('windowMetrics parses the native response', function () {
        $GLOBALS['__nativephp_mock']['Emulator.WindowMetrics'] = json_encode([
            'width' => 874, 'height' => 402, 'top' => 0, 'bottom' => 21, 'left' => 59, 'right' => 59,
        ]);
        expect(Emulator::windowMetrics())->toBe([
            'width' => 874, 'height' => 402, 'top' => 0, 'bottom' => 21, 'left' => 59, 'right' => 59,
        ]);
    });

    it('windowMetrics returns zeros on a response without metrics', function () {
        $GLOBALS['__nativephp_mock']['Emulator.WindowMetrics'] = '{"status":"error"}';
        expect(Emulator::windowMetrics())->toBe([
            'width' => 0, 'height' => 0, 'top' => 0, 'bottom' => 0, 'left' => 0, 'right' => 0,
        ]);
    });

    it('surface returns Emulator instance after successful native call', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Boot'] = '{"status":"bound","surface":"main"}';
        $emu = Emulator::surface('main');
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('loadRom returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.LoadRom'] = '{"status":"loading","path":"/path/to/rom.sfc"}';
        $emu = (new Emulator)->loadRom('/path/to/rom.sfc');
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('pause returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Pause'] = '{"status":"paused"}';
        $emu = Emulator::surface()->pause();
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('watchMemory returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.WatchMemory'] = '{"status":"watching","count":2}';
        $emu = Emulator::surface()->watchMemory(0x7E0010)->watchMemory(0x7EF340, length: 2);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('fastForward returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.FastForward'] = '{"status":"fast"}';
        $emu = Emulator::surface()->fastForward(true);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('toggleRewind returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ToggleRewind'] = '{"status":"rewinding"}';
        $emu = Emulator::surface()->toggleRewind();
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('pickRom sends destination and extensions, returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.PickRom'] = '{"status":"picking"}';
        $emu = Emulator::surface()->pickRom('/tmp/roms/sfc', ['sfc', 'smc']);
        expect($emu)->toBeInstanceOf(Emulator::class);

        $call = collect($GLOBALS['__nativephp_calls'])->last(
            fn ($c) => $c['function'] === 'Emulator.PickRom',
        );
        expect($call['payload']['destination'])->toBe('/tmp/roms/sfc')
            ->and($call['payload']['extensions'])->toBe(['sfc', 'smc']);
    });

    it('rewind sends the seconds and returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Rewind'] = '{"jumped":10}';
        $emu = Emulator::surface()->rewind(10);
        expect($emu)->toBeInstanceOf(Emulator::class);

        $call = collect($GLOBALS['__nativephp_calls'])->last(
            fn ($c) => $c['function'] === 'Emulator.Rewind',
        );
        expect($call['payload']['seconds'])->toBe(10);
    });

    it('rewind throws REWIND_DISABLED when capture is off', function () {
        $GLOBALS['__nativephp_mock']['Emulator.Rewind'] =
            '{"status":"error","code":"REWIND_DISABLED","message":"Rewind capture is off"}';
        Emulator::surface()->rewind();
    })->throws(KevinBatdorf\RetroEmulator\EmulatorException::class);

    it('saveState returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.StateSave'] = '{"status":"saved","slot":1}';
        $emu = Emulator::surface()->saveState(1);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });

    it('loadState returns fluent instance', function () {
        $GLOBALS['__nativephp_mock']['Emulator.StateLoad'] = '{"status":"loaded","slot":1}';
        $emu = Emulator::surface()->loadState(1);
        expect($emu)->toBeInstanceOf(Emulator::class);
    });
});

describe('loadCheatsFile', function () {
    beforeEach(function () {
        $GLOBALS['__nativephp_calls'] = [];
        $this->cheatsFile = sys_get_temp_dir().'/'.uniqid('pest-cheats-').'.cheats.bml';
    });

    afterEach(function () {
        if (file_exists($this->cheatsFile)) {
            unlink($this->cheatsFile);
        }
    });

    it('registers enabled cheats and skips disabled or codeless entries', function () {
        file_put_contents($this->cheatsFile, implode("\n", [
            'cheats',
            '  revision: 2026-07-11',
            '',
            'cheat',
            '  description: Infinite health',
            '  code: 7E0010:01+7E0011:FF',
            '  enabled: true',
            '',
            'cheat',
            '  description: Disabled cheat',
            '  code: 7E0020:63',
            '  enabled: false',
            '',
            'cheat',
            '  description: No code',
            '  enabled: true',
            '',
        ]));

        (new Emulator)->loadCheatsFile($this->cheatsFile);

        $added = array_values(array_filter(
            $GLOBALS['__nativephp_calls'],
            fn (array $call) => $call['function'] === 'Emulator.AddCheat',
        ));

        expect($added)->toHaveCount(1);
        expect($added[0]['payload']['code'])->toBe('7E0010:01+7E0011:FF');
        expect($added[0]['payload']['description'])->toBe('Infinite health');
    });

    it('returns fluent instance for an empty cheats file', function () {
        file_put_contents($this->cheatsFile, "cheats\n  revision: 2026-07-11\n");

        expect((new Emulator)->loadCheatsFile($this->cheatsFile))->toBeInstanceOf(Emulator::class);
        expect($GLOBALS['__nativephp_calls'])->toBe([]);
    });

    it('throws when the file is not readable', function () {
        (new Emulator)->loadCheatsFile('/nonexistent/path.cheats.bml');
    })->throws(RuntimeException::class);
});

describe('Typed layer', function () {
    beforeEach(function () {
        $GLOBALS['__nativephp_calls'] = [];
    });

    it('button enums match the native registry', function (string $systemId, string $enumClass) {
        // Every system's `.id` entry is in this one file, so one regex covers all.
        $registry = file_get_contents(dirname(__DIR__).'/native/host/system_catalog.cpp');

        preg_match(
            '/\.id\s*=\s*"'.$systemId.'".*?\.buttons\s*=\s*\{(.*?)\n\s*\},/s',
            $registry,
            $m,
        );
        expect($m)->not->toBe([], "no .buttons block found for '{$systemId}'");

        preg_match_all('/\{"([^"]+)",/', $m[1], $names);
        $native = $names[1];
        $enum = array_map(fn ($case) => $case->value, $enumClass::cases());

        sort($native);
        sort($enum);
        expect($enum)->toBe($native);
    })->with([
        ['sfc', SfcButton::class],
        ['fc', FcButton::class],
        ['gb', GbButton::class],
        ['gbc', GbButton::class],
        ['md', MdButton::class],
        ['gba', GbaButton::class],
    ]);

    it('system enum matches the ids GetSystems reports', function () {
        $kotlin = file_get_contents(dirname(__DIR__).'/resources/android/src/EmulatorFunctions.kt');

        preg_match_all('/system\("([a-z0-9]+)",\s*"/', $kotlin, $m);
        $native = $m[1];
        $enum = array_map(fn ($case) => $case->value, System::cases());

        sort($native);
        sort($enum);
        expect($enum)->toBe($native);
    });

    it('backend enum matches the native discovery list', function () {
        $jni = file_get_contents(dirname(__DIR__).'/android/app/src/main/cpp/emulator_jni.cpp');

        preg_match('/kBackendNames\[\]\s*=\s*\{(.*?)\};/s', $jni, $m);
        preg_match_all('/"([a-z0-9]+)"/', $m[1] ?? '', $names);
        $native = $names[1];
        $enum = array_map(fn ($case) => $case->value, Backend::cases());

        sort($native);
        sort($enum);
        expect($enum)->toBe($native);
    });

    it('serializes the backend choice as its wire string', function () {
        $config = new GbConfig(backend: Backend::Ares);

        expect($config->toArray()['backend'])->toBe('ares');
        expect((new GbConfig)->toArray())->not->toHaveKey('backend');
        expect((new GbConfig(backend: 'quicknes'))->toArray()['backend'])->toBe('quicknes');
    });

    it('serializes engineOptions as declared value strings and omits when empty', function () {
        $config = new SfcConfig(backend: 'snes9x', engineOptions: [
            'snes9x_overclock' => '150%',
            'snes9x_randomize_memory' => 1,
        ]);

        expect($config->toArray()['engineOptions'])->toBe([
            'snes9x_overclock' => '150%',
            'snes9x_randomize_memory' => '1',
        ]);
        expect((new SfcConfig)->toArray())->not->toHaveKey('engineOptions');
    });

    it('rejects engineOptions values that are not value strings', function () {
        new SfcConfig(engineOptions: ['snes9x_overscan' => true]);
    })->throws(InvalidArgumentException::class);

    it('routes runtime engineOptions through Configure', function () {
        $GLOBALS['__nativephp_calls'] = [];

        Emulator::surface('main')->configure(['engineOptions' => ['snes9x_region' => 'pal']]);

        $call = end($GLOBALS['__nativephp_calls']);
        expect($call['function'])->toBe('Emulator.Configure');
        expect($call['payload']['options']['engineOptions'])->toBe(['snes9x_region' => 'pal']);
    });

    it('lists the engine-declared schema via engineOptions()', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetEngineOptions'] = json_encode([
            'status' => 'ok',
            'options' => [[
                'key' => 'snes9x_region',
                'choices' => ['auto', 'ntsc', 'pal'],
                'default' => 'auto',
                'current' => 'pal',
            ]],
        ]);

        try {
            $options = Emulator::surface('main')->engineOptions();

            expect($options)->toHaveCount(1);
            expect($options[0]['key'])->toBe('snes9x_region');
            expect($options[0]['choices'])->toBe(['auto', 'ntsc', 'pal']);
            expect($options[0]['current'])->toBe('pal');
        } finally {
            unset($GLOBALS['__nativephp_mock']['Emulator.GetEngineOptions']);
        }
    });

    it('names a licence for every core in the verified table', function () {
        foreach (['snes9x', 'bsnes', 'fceumm', 'mesen', 'picodrive', 'genesis_plus_gx'] as $core) {
            expect(CopyAssetsCommand::coreLicenceNote("{$core}_libretro_android.so"))
                ->not->toContain('licence unknown');
        }

        expect(CopyAssetsCommand::coreLicenceNote('mystery_core.so'))
            ->toContain('licence unknown');
    });

    it('resolves the engine: explicit is strict, the app map is a graceful preference list', function () {
        $GLOBALS['__nativephp_calls'] = [];
        config()->set('retro-emulator.backends', [
            'gb' => 'ares',
            'fc' => ['fceumm', Backend::Ares],
        ]);

        try {
            Emulator::surface('main')->loadSystem(System::Gb);
            Emulator::surface('main')->loadSystem(System::Gb, new GbConfig(backend: Backend::SameBoy));
            Emulator::surface('main')->loadSystem(System::Fc);
            Emulator::surface('main')->loadSystem(System::Sfc);

            $staged = array_values(array_filter(
                $GLOBALS['__nativephp_calls'],
                fn ($call) => $call['function'] === 'Emulator.LoadSystem',
            ));

            // The map travels as preferences (a string wraps to a list, enum
            // cases become wire strings); explicit stays the strict backend
            // key; unmapped systems send nothing — an unnamed boot runs the
            // built-in engine.
            expect($staged[0]['payload']['config']['backendPreferences'])->toBe(['ares']);
            expect($staged[0]['payload']['config'])->not->toHaveKey('backend');
            expect($staged[1]['payload']['config']['backend'])->toBe('sameboy');
            expect($staged[1]['payload']['config'])->not->toHaveKey('backendPreferences');
            expect($staged[2]['payload']['config']['backendPreferences'])->toBe(['fceumm', 'ares']);
            expect($staged[3]['payload']['config'])->not->toHaveKey('backend');
            expect($staged[3]['payload']['config'])->not->toHaveKey('backendPreferences');
        } finally {
            config()->set('retro-emulator.backends', []);
        }
    });

    it('warns in the log when a boot lands off the preferred engine', function () {
        $log = new class extends AbstractLogger
        {
            /** @var string[] */
            public array $warnings = [];

            public function log($level, Stringable|string $message, array $context = []): void
            {
                if ($level === 'warning') {
                    $this->warnings[] = (string) $message;
                }
            }
        };
        Container::getInstance()->instance('log', $log);
        $GLOBALS['__nativephp_mock']['Emulator.LoadSystem'] = json_encode([
            'status' => 'staged', 'system' => 'sfc', 'backend' => 'ares',
        ]);
        config()->set('retro-emulator.backends', ['sfc' => ['snes9x', 'bsnes']]);

        try {
            Emulator::surface('main')->loadSystem(System::Sfc);

            expect($log->warnings)->toHaveCount(1);
            expect($log->warnings[0])->toContain("running 'ares'")
                ->toContain('fetch-core snes9x');

            $GLOBALS['__nativephp_mock']['Emulator.LoadSystem'] = json_encode([
                'status' => 'staged', 'system' => 'sfc', 'backend' => 'snes9x',
            ]);
            Emulator::surface('main')->loadSystem(System::Sfc);
            expect($log->warnings)->toHaveCount(1);
        } finally {
            config()->set('retro-emulator.backends', []);
            unset($GLOBALS['__nativephp_mock']['Emulator.LoadSystem']);
            Container::getInstance()->forgetInstance('log');
        }
    });

    it('ships a performance-first preference map covering every system', function () {
        $map = require dirname(__DIR__).'/config/retro-emulator.php';

        foreach (array_map(fn ($case) => $case->value, System::cases()) as $id) {
            expect($map['backends'])->toHaveKey($id);
            // One shape for every console: [fast pick, accurate pick].
            expect($map['backends'][$id])->toBeArray()->toHaveCount(2);
        }

        expect($map['backends']['gb'][0])->toBe('sameboy');
        expect($map['backends']['gbc'][0])->toBe('sameboy');
        expect($map['backends']['gba'][0])->toBe('mgba');
    });

    it('config classes send only explicitly set options', function () {
        $config = new SfcConfig(autoSave: false, rewind: true);

        expect($config->toArray())->toBe(['autoSave' => false, 'rewind' => true]);
    });

    it('config classes can opt out of dynamic rate control', function () {
        $config = new SfcConfig(dynamicRateControl: false);

        expect($config->toArray())->toBe(['dynamicRateControl' => false]);
    });

    it('regional configs send region knobs as wire strings', function () {
        $config = new SfcConfig(
            region: Region::Pal,
            preferredRegions: [Region::NtscJ, Region::Pal],
        );

        expect($config->toArray())->toBe([
            'region' => 'PAL',
            'preferredRegions' => ['NTSC-J', 'PAL'],
        ]);
    });

    it('regional configs omit region knobs unless set', function () {
        expect((new MdConfig(autoSave: true))->toArray())->toBe(['autoSave' => true]);
    });

    it('the region-free GbConfig has no region knobs', function () {
        expect(is_subclass_of(GbConfig::class, RegionalSystemConfig::class))->toBeFalse();
    });

    it('SfcConfig carries deepBlackBoost as a per-system toggle', function () {
        expect((new SfcConfig(deepBlackBoost: true))->toArray())->toBe(['deepBlackBoost' => true]);
    });

    it('GbConfig carries the Game Boy display toggles', function () {
        $config = new GbConfig(colorEmulation: true, interframeBlending: true);

        expect($config->toArray())->toBe([
            'colorEmulation' => true,
            'interframeBlending' => true,
        ]);
    });

    it('GbaConfig carries the same display toggles for ares boots', function () {
        $config = new GbaConfig(colorEmulation: true, interframeBlending: true);

        expect($config->toArray())->toBe([
            'colorEmulation' => true,
            'interframeBlending' => true,
        ]);
        expect((new GbaConfig)->toArray())->toBe([]);
    });

    it('the accuracy preset resolves to the pixelAccuracy wire flag', function () {
        expect((new SfcConfig(accuracy: Accuracy::Accurate))->toArray())
            ->toBe(['pixelAccuracy' => true]);
        expect((new GbaConfig(accuracy: Accuracy::Performance))->toArray())
            ->toBe(['pixelAccuracy' => false]);
    });

    it('a direct pixelAccuracy override beats the preset', function () {
        $config = new SfcConfig(accuracy: Accuracy::Accurate, pixelAccuracy: false);

        expect($config->toArray())->toBe(['pixelAccuracy' => false]);
    });

    it('accuracy left unset stays off the wire, keeping the native default', function () {
        expect((new SfcConfig)->toArray())->toBe([]);
    });

    it('every system config accepts the accuracy knobs', function () {
        $configs = [FcConfig::class, GbConfig::class, GbaConfig::class, MdConfig::class, SfcConfig::class];

        foreach ($configs as $class) {
            expect((new $class(accuracy: Accuracy::Accurate))->toArray())
                ->toBe(['pixelAccuracy' => true], $class);
        }
    });

    it('treats BOOT_ONLY_OPTION as a programmer error that throws', function () {
        expect(EmulatorErrorCode::BootOnlyOption->throwsAsException())->toBeTrue();
    });

    it('the global Config serializes shared knobs as wire values', function () {
        $config = new Config(
            luminance: 100,
            output: VideoOutput::Integer,
            aspectCorrection: AspectCorrection::None,
            volume: 80,
            inputCapture: InputCapture::Global,
            speed: 1.5,
            rewind: true,
        );

        expect($config->toArray())->toBe([
            'luminance' => 100,
            'output' => 'integer',
            'aspectCorrection' => 'none',
            'volume' => 80,
            'inputCapture' => 'global',
            'speed' => 1.5,
            'rewind' => true,
        ]);
    });

    it('the global Config omits every unset knob', function () {
        expect((new Config)->toArray())->toBe([]);
    });

    it('a system config inherits and overrides the shared Config knobs', function () {
        $config = new SfcConfig(luminance: 90, volume: 60, deepBlackBoost: true);

        expect($config->toArray())->toBe([
            'luminance' => 90,
            'volume' => 60,
            'deepBlackBoost' => true,
        ]);
    });

    it('loadSystem accepts a System enum and a config object', function () {
        Emulator::surface()->loadSystem(
            System::Sfc,
            new SfcConfig(rewind: true, rewindBufferSeconds: 30),
        );

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.LoadSystem');
        expect($call['payload']['system'])->toBe('sfc');
        expect($call['payload']['config'])->toBe(['rewind' => true, 'rewindBufferSeconds' => 30]);
    });

    it('loadSystem fans a config\'s AV knobs out to their own setters', function () {
        Emulator::surface()->loadSystem(
            System::Sfc,
            new SfcConfig(luminance: 120, volume: 80, deepBlackBoost: true, rewind: true),
        );

        $calls = collect($GLOBALS['__nativephp_calls']);

        expect($calls->firstWhere('function', 'Emulator.LoadSystem')['payload']['config'])
            ->toBe(['rewind' => true, 'deepBlackBoost' => true]);

        expect($calls->firstWhere('function', 'Emulator.SetVideo')['payload']['options'])
            ->toBe(['luminance' => 120]);
        expect($calls->firstWhere('function', 'Emulator.SetAudio')['payload']['options'])
            ->toBe(['volume' => 80]);
    });

    it('loadSystem leaves inputCapture for surface creation, not the load path', function () {
        Emulator::surface()->loadSystem(System::Sfc, new SfcConfig(inputCapture: InputCapture::Global));

        $calls = collect($GLOBALS['__nativephp_calls']);
        expect($calls->firstWhere('function', 'Emulator.LoadSystem')['payload']['config'])->toBe([]);
        expect($calls->firstWhere('function', 'Emulator.SetVideo'))->toBeNull();
    });

    it('connectDevice registers a device and returns a Controller for the port', function () {
        $controller = Emulator::surface()->connectDevice(2, Device::Mouse);

        expect($controller)->toBeInstanceOf(Controller::class);
        expect($controller->port)->toBe(2);
        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.ConnectDevice');
        expect($call['payload']['port'])->toBe(2);
        expect($call['payload']['device'])->toBe('Mouse');
    });

    it('connectMultitap returns four Controllers for a multitap', function () {
        $GLOBALS['__nativephp_mock']['Emulator.ConnectDevice'] =
            '{"status":"connected","port":2,"device":"Super Multitap","ports":[2,3,4,5]}';

        $players = Emulator::surface()->connectMultitap(2, Device::SuperMultitap);

        expect($players)->toBeArray()->toHaveCount(4);
        expect(array_map(fn ($c) => $c->port, $players))->toBe([2, 3, 4, 5]);
        expect($players[2])->toBeInstanceOf(Controller::class);
    });

    it('the handle presses a button on its port', function () {
        Emulator::surface()->getDevice(1)->press(SfcButton::A);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.PressButton');
        expect($call['payload']['port'])->toBe(1);
        expect($call['payload']['button'])->toBe('A');
    });

    it('the handle reports the buttons held on its port', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetPressedButtons'] = json_encode([
            'port' => 1, 'buttons' => ['Up', 'A'],
        ]);

        $pressed = Emulator::surface()->getDevice(1)->pressed();

        expect($pressed)->toBe(['Up', 'A']);
        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.GetPressedButtons');
        expect($call['payload']['port'])->toBe(1);
    });

    it('the handle reports an empty list when nothing is held', function () {
        $GLOBALS['__nativephp_mock']['Emulator.GetPressedButtons'] = json_encode([
            'port' => 1, 'buttons' => [],
        ]);

        expect(Emulator::surface()->getDevice(1)->pressed())->toBe([]);
    });

    it('the handle sets an atomic button snapshot', function () {
        Emulator::surface()->getDevice(1)->setButtons(['Up' => true, 'A' => true]);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetButtons');
        expect($call['payload']['state'])->toBe(['Up' => true, 'A' => true]);
    });

    it('the handle feeds a relative axis delta', function () {
        Emulator::surface()->getDevice(1)->setAxis('X', -12);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetAxis');
        expect($call['payload']['port'])->toBe(1);
        expect($call['payload']['axis'])->toBe('X');
        expect($call['payload']['value'])->toBe(-12);
    });

    it('the handle aims an axis device at a normalized position', function () {
        Emulator::surface()->getDevice(2)->aimAt(0.25, 0.75);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.AimAt');
        expect($call['payload']['port'])->toBe(2);
        expect($call['payload']['x'])->toBe(0.25);
        expect($call['payload']['y'])->toBe(0.75);
    });

    it('the handle remaps buttons on its port', function () {
        Emulator::surface()->getDevice(1)->remap(['a' => 'b', 'b' => 'a']);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetInputMapping');
        expect($call['payload']['port'])->toBe(1);
        expect($call['payload']['mappings'])->toBe(['a' => 'b', 'b' => 'a']);
    });

    it('the handle resets a remap with an empty map', function () {
        Emulator::surface()->getDevice(2)->remap([]);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetInputMapping');
        expect($call['payload']['port'])->toBe(2);
        expect($call['payload']['mappings'])->toBe([]);
    });

    it('watchMemory sends a single address entry with length', function () {
        Emulator::surface()->watchMemory(0x7EF340, length: 2);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.WatchMemory');
        expect($call['payload']['addresses'])->toBe([['address' => 0x7EF340, 'length' => 2]]);
    });

    it('loadRom stages slots then loads the base for slotted media', function () {
        Emulator::surface()->loadRom([
            'base' => '/roms/sufami.sfc',
            'slotA' => '/roms/game-a.st',
            'slotB' => '/roms/game-b.st',
        ]);

        $calls = collect($GLOBALS['__nativephp_calls']);
        $slots = $calls->where('function', 'Emulator.StageSlot')->values();
        expect($slots)->toHaveCount(2);
        expect($slots[0]['payload'])->toBe(['surface' => 'main', 'index' => 0, 'path' => '/roms/game-a.st']);
        expect($slots[1]['payload'])->toBe(['surface' => 'main', 'index' => 1, 'path' => '/roms/game-b.st']);
        $load = $calls->firstWhere('function', 'Emulator.LoadRom');
        expect($load['payload']['path'])->toBe('/roms/sufami.sfc');
    });

    it('loadRom omits savePath unless given', function () {
        Emulator::surface()->loadRom('/roms/a.sfc');
        Emulator::surface()->loadRom('/roms/b.sfc', savePath: '/saves/b');

        $calls = array_values(array_filter(
            $GLOBALS['__nativephp_calls'],
            fn (array $call) => $call['function'] === 'Emulator.LoadRom',
        ));
        expect($calls[0]['payload'])->not->toHaveKey('savePath');
        expect($calls[1]['payload']['savePath'])->toBe('/saves/b');
    });

    it('setVideo sends only the named options', function () {
        Emulator::surface()->setVideo(luminance: 90, colorBleed: true);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetVideo');
        expect($call['payload']['options'])->toBe(['luminance' => 90, 'colorBleed' => true]);
    });

    it('sends gamma as a whole percentage, like the options beside it', function () {
        // A float here means the caller thinks it is ares' 1.0-2.0 exponent; the
        // public contract is a percentage so all three picture knobs match.
        Emulator::surface()->setVideo(luminance: 90, saturation: 80, gamma: 150);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetVideo');
        expect($call['payload']['options'])->toBe([
            'luminance' => 90, 'saturation' => 80, 'gamma' => 150,
        ]);
    });

    it('types every percentage option as an int', function () {
        $video = new ReflectionMethod(Emulator::class, 'setVideo');
        $types = collect($video->getParameters())
            ->filter(fn ($p) => in_array($p->getName(), ['luminance', 'saturation', 'gamma'], true))
            ->mapWithKeys(fn ($p) => [$p->getName() => (string) $p->getType()]);

        expect($types->all())->toBe([
            'luminance' => '?int', 'saturation' => '?int', 'gamma' => '?int',
        ]);
    });

    it('carries gamma through a Config as an int percentage', function () {
        expect((new Config(gamma: 150))->toArray())->toBe(['gamma' => 150]);

        $gamma = (new ReflectionClass(Config::class))->getConstructor()->getParameters();
        $type = collect($gamma)->firstWhere(fn ($p) => $p->getName() === 'gamma')->getType();
        expect((string) $type)->toBe('?int');
    });

    it('setSystemOptions sends per-system toggles', function () {
        Emulator::surface()->setSystemOptions(['deepBlackBoost' => true]);

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetSystemOptions');
        expect($call['payload']['options'])->toBe(['deepBlackBoost' => true]);
    });

    it('setVideo sends presentation settings as wire strings', function () {
        Emulator::surface()->setVideo(
            output: VideoOutput::IntegerFixed,
            fixedScale: 3,
            aspectCorrection: AspectCorrection::None,
        );

        $call = collect($GLOBALS['__nativephp_calls'])->firstWhere('function', 'Emulator.SetVideo');
        expect($call['payload']['options'])->toBe([
            'output' => 'integerFixed', 'fixedScale' => 3, 'aspectCorrection' => 'none',
        ]);
    });
});
