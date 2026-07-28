<?php

use KevinBatdorf\RetroEmulator\Components\Dpad as DpadComponent;
use KevinBatdorf\RetroEmulator\Components\Emulator as EmulatorComponent;
use KevinBatdorf\RetroEmulator\Config\Config;
use KevinBatdorf\RetroEmulator\Config\GbConfig;
use KevinBatdorf\RetroEmulator\Config\SfcConfig;
use KevinBatdorf\RetroEmulator\Elements\Dpad as DpadElement;
use KevinBatdorf\RetroEmulator\Elements\Emulator as EmulatorElement;
use KevinBatdorf\RetroEmulator\InputCapture;
use KevinBatdorf\RetroEmulator\System;
use Native\Mobile\Edge\CallbackRegistry;
use Native\Mobile\Edge\Element;
use Native\Mobile\Edge\ElementRegistry;
use Native\Mobile\Edge\NativeElementCollector;

describe('EDGE element contract', function () {
    it('extends the Edge Element base with the emulator type', function () {
        $element = new EmulatorElement;

        expect($element)->toBeInstanceOf(Element::class);
        expect($element->getType())->toBe('emulator');
    });

    it('serializes default props into the node', function () {
        $node = (new EmulatorElement)->toArray(new CallbackRegistry);

        expect($node['type'])->toBe('emulator');
        expect($node['props'])->toBe(['name' => 'main', 'z_index' => 0, 'input_capture' => 'focus']);
    });

    it('maps Blade attributes to props', function () {
        $element = new EmulatorElement;
        $element->applyAttributes(['name' => 'side', 'z-index' => '2', 'input-capture' => 'global']);

        $node = $element->toArray(new CallbackRegistry);

        expect($node['props'])->toBe(['name' => 'side', 'z_index' => 2, 'input_capture' => 'global']);
    });

    it('ignores an unknown input-capture attribute, keeping the default', function () {
        $element = new EmulatorElement;
        $element->applyAttributes(['input-capture' => 'bogus']);

        expect($element->toArray(new CallbackRegistry)['props']['input_capture'])->toBe('focus');
    });

    it('supports fluent construction', function () {
        $node = EmulatorElement::make('side')
            ->zIndex(1)
            ->inputCapture(InputCapture::Global)
            ->toArray(new CallbackRegistry);

        expect($node['props'])->toBe(['name' => 'side', 'z_index' => 1, 'input_capture' => 'global']);
    });

    it('participates in layout like any Edge node', function () {
        $node = EmulatorElement::make()->fill()->toArray(new CallbackRegistry);

        expect($node['layout'])->toBe(['width' => 'fill', 'height' => 'fill']);
    });

    it('serializes declarative setup props (system, config, rom)', function () {
        $node = EmulatorElement::make('main')
            ->system(System::Sfc)
            ->config(new Config(volume: 80))
            ->rom('/roms/game.sfc')
            ->toArray(new CallbackRegistry);

        expect($node['props']['system'])->toBe('sfc');
        expect(json_decode($node['props']['config'], true))->toBe(['volume' => 80]);
        expect($node['props']['rom'])->toBe('/roms/game.sfc');
    });

    it('serializes config as a wire-safe JSON string, not a nested map', function () {
        // The EDGE wire format carries only scalar props; a nested array would
        // never reach native, so config must cross as a string.
        $config = (new EmulatorElement)
            ->config(new Config(volume: 80))
            ->toArray(new CallbackRegistry)['props']['config'];

        expect($config)->toBeString();
        expect(json_decode($config, true))->toBe(['volume' => 80]);
    });

    it('merges the global config under the system config, system winning', function () {
        $node = EmulatorElement::make()
            ->config(new Config(volume: 80, luminance: 100))
            ->systemConfig(new SfcConfig(volume: 50, deepBlackBoost: true))
            ->toArray(new CallbackRegistry);

        expect(json_decode($node['props']['config'], true))->toBe([
            'luminance' => 100,
            'volume' => 50,
            'deepBlackBoost' => true,
        ]);
    });

    it('hoists a config inputCapture onto the surface prop, not the config map', function () {
        $node = EmulatorElement::make()
            ->config(new Config(inputCapture: InputCapture::Global, volume: 70))
            ->toArray(new CallbackRegistry);

        expect($node['props']['input_capture'])->toBe('global');
        expect(json_decode($node['props']['config'], true))->toBe(['volume' => 70]);
    });

    it('maps object config attributes to serialized props', function () {
        $element = new EmulatorElement;
        $element->applyAttributes([
            'system' => 'gb',
            'config' => new Config(rewind: true),
            'system-config' => new GbConfig(colorEmulation: true),
            'rom' => '/roms/x.gb',
        ]);

        $props = $element->toArray(new CallbackRegistry)['props'];
        expect($props['system'])->toBe('gb');
        expect(json_decode($props['config'], true))->toBe(['rewind' => true, 'colorEmulation' => true]);
        expect($props['rom'])->toBe('/roms/x.gb');
    });
});

describe('Blade component emits the element', function () {
    beforeEach(function () {
        ElementRegistry::reset();
        ElementRegistry::register('emulator', EmulatorElement::class);
        NativeElementCollector::setCallbacks(new CallbackRegistry);
    });

    it('renders as an emulator element, not HTML', function () {
        $component = new EmulatorComponent(name: 'side', zIndex: 1);
        $html = ($component->render())();

        expect($html)->toBe('');

        $root = NativeElementCollector::collect();
        $node = $root->toArray(new CallbackRegistry);

        // collect() wraps roots in a column when needed; find our node.
        $emulator = $node['type'] === 'emulator'
            ? $node
            : collect($node['children'] ?? [])->firstWhere('type', 'emulator');

        expect($emulator)->not->toBeNull();
        expect($emulator['props']['name'])->toBe('side');
        expect($emulator['props']['z_index'])->toBe(1);
    });

    it('forwards declarative setup props from the component tag', function () {
        $component = new EmulatorComponent(
            name: 'side',
            system: System::Gb,
            config: new Config(volume: 60),
            rom: '/roms/y.gb',
        );
        ($component->render())();

        $node = NativeElementCollector::collect()->toArray(new CallbackRegistry);
        $emulator = $node['type'] === 'emulator'
            ? $node
            : collect($node['children'] ?? [])->firstWhere('type', 'emulator');

        expect($emulator['props']['system'])->toBe('gb');
        expect(json_decode($emulator['props']['config'], true))->toBe(['volume' => 60]);
        expect($emulator['props']['rom'])->toBe('/roms/y.gb');
    });
});

describe('Dpad element contract', function () {
    it('extends the Edge Element base with the dpad type', function () {
        $element = new DpadElement;

        expect($element)->toBeInstanceOf(Element::class);
        expect($element->getType())->toBe('dpad');
    });

    it('serializes default props into the node', function () {
        $node = (new DpadElement)->toArray(new CallbackRegistry);

        expect($node['type'])->toBe('dpad');
        expect($node['props'])->toBe(['surface' => 'main', 'port' => 1]);
    });

    it('leaves feel props absent so each renderer applies its own default', function () {
        $props = (new DpadElement)->toArray(new CallbackRegistry)['props'];

        expect($props)->not->toHaveKey('threshold');
        expect($props)->not->toHaveKey('diagonal_ratio');
        expect($props)->not->toHaveKey('color');
        expect($props)->not->toHaveKey('active_color');
    });

    it('maps Blade attributes to props', function () {
        $element = new DpadElement;
        $element->applyAttributes([
            'surface' => 'side',
            'port' => '4',
            'threshold' => '0.4',
            'diagonal-ratio' => '0.5',
            'color' => '#40FFFFFF',
            'active-color' => '#FFFFFFFF',
        ]);

        expect($element->toArray(new CallbackRegistry)['props'])->toBe([
            'surface' => 'side',
            'port' => 4,
            'threshold' => 0.4,
            'diagonal_ratio' => 0.5,
            'color' => '#40FFFFFF',
            'active_color' => '#FFFFFFFF',
        ]);
    });

    it('accepts camelCase attributes too', function () {
        $element = new DpadElement;
        $element->applyAttributes(['diagonalRatio' => '0.6', 'activeColor' => '#FF0000FF']);

        $props = $element->toArray(new CallbackRegistry)['props'];
        expect($props['diagonal_ratio'])->toBe(0.6);
        expect($props['active_color'])->toBe('#FF0000FF');
    });

    it('supports fluent construction', function () {
        $node = DpadElement::make('side')
            ->port(2)
            ->threshold(0.25)
            ->diagonalRatio(0.7)
            ->color('#20FFFFFF')
            ->activeColor('#F0FFFFFF')
            ->toArray(new CallbackRegistry);

        expect($node['props'])->toBe([
            'surface' => 'side',
            'port' => 2,
            'threshold' => 0.25,
            'diagonal_ratio' => 0.7,
            'color' => '#20FFFFFF',
            'active_color' => '#F0FFFFFF',
        ]);
    });

    it('participates in layout like any Edge node', function () {
        $node = DpadElement::make()->fill()->toArray(new CallbackRegistry);

        expect($node['layout'])->toBe(['width' => 'fill', 'height' => 'fill']);
    });

    it('registers no callbacks — presses never reach PHP', function () {
        $registry = new CallbackRegistry;
        $props = DpadElement::make()->port(3)->toArray($registry)['props'];

        expect(array_filter(array_keys($props), fn ($k) => str_starts_with($k, 'on_')))->toBe([]);
    });
});

describe('Dpad blade component emits the element', function () {
    beforeEach(function () {
        ElementRegistry::reset();
        ElementRegistry::register('dpad', DpadElement::class);
        NativeElementCollector::setCallbacks(new CallbackRegistry);
    });

    it('renders as a dpad element, not HTML', function () {
        $component = new DpadComponent(surface: 'side', port: 2, threshold: 0.4);
        $html = ($component->render())();

        expect($html)->toBe('');

        $node = NativeElementCollector::collect()->toArray(new CallbackRegistry);
        $dpad = $node['type'] === 'dpad'
            ? $node
            : collect($node['children'] ?? [])->firstWhere('type', 'dpad');

        expect($dpad)->not->toBeNull();
        expect($dpad['props']['surface'])->toBe('side');
        expect($dpad['props']['port'])->toBe(2);
        expect($dpad['props']['threshold'])->toBe(0.4);
    });

    it('omits unset feel props so the tag forms stay interchangeable', function () {
        ((new DpadComponent)->render())();

        $node = NativeElementCollector::collect()->toArray(new CallbackRegistry);
        $dpad = $node['type'] === 'dpad'
            ? $node
            : collect($node['children'] ?? [])->firstWhere('type', 'dpad');

        expect($dpad['props'])->toBe(['surface' => 'main', 'port' => 1]);
    });
});
