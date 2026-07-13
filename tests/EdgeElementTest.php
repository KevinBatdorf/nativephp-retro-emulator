<?php

use KevinBatdorf\RetroEmulator\Components\Emulator as EmulatorComponent;
use KevinBatdorf\RetroEmulator\Config\Config;
use KevinBatdorf\RetroEmulator\Config\GbConfig;
use KevinBatdorf\RetroEmulator\Config\SfcConfig;
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
        expect($node['props']['config'])->toBe(['volume' => 80]);
        expect($node['props']['rom'])->toBe('/roms/game.sfc');
    });

    it('merges the global config under the system config, system winning', function () {
        $node = EmulatorElement::make()
            ->config(new Config(volume: 80, luminance: 100))
            ->systemConfig(new SfcConfig(volume: 50, deepBlackBoost: true))
            ->toArray(new CallbackRegistry);

        expect($node['props']['config'])->toBe([
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
        expect($node['props']['config'])->toBe(['volume' => 70]);
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
        expect($props['config'])->toBe(['rewind' => true, 'colorEmulation' => true]);
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
        expect($emulator['props']['config'])->toBe(['volume' => 60]);
        expect($emulator['props']['rom'])->toBe('/roms/y.gb');
    });
});
