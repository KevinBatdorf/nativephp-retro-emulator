<?php

use KevinBatdorf\RetroEmulator\Components\Emulator as EmulatorComponent;
use KevinBatdorf\RetroEmulator\Elements\Emulator as EmulatorElement;
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
        expect($node['props'])->toBe(['name' => 'main', 'z_index' => 0]);
    });

    it('maps Blade attributes to props', function () {
        $element = new EmulatorElement;
        $element->applyAttributes(['name' => 'side', 'z-index' => '2']);

        $node = $element->toArray(new CallbackRegistry);

        expect($node['props'])->toBe(['name' => 'side', 'z_index' => 2]);
    });

    it('supports fluent construction', function () {
        $node = EmulatorElement::make('side')->zIndex(1)->toArray(new CallbackRegistry);

        expect($node['props'])->toBe(['name' => 'side', 'z_index' => 1]);
    });

    it('participates in layout like any Edge node', function () {
        $node = EmulatorElement::make()->fill()->toArray(new CallbackRegistry);

        expect($node['layout'])->toBe(['width' => 'fill', 'height' => 'fill']);
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
});
