<?php

use KevinBatdorf\RetroEmulator\Emulator;
use KevinBatdorf\RetroEmulator\EmulatorErrorCode;
use KevinBatdorf\RetroEmulator\Shaders;

afterEach(function () {
    $GLOBALS['__nativephp_mock'] = [];
    $GLOBALS['__nativephp_calls'] = [];
});

it('lists .slangp presets recursively as sorted absolute paths', function () {
    $dir = sys_get_temp_dir().'/shaders_'.uniqid();
    mkdir($dir.'/crt', 0777, true);
    touch($dir.'/b.slangp');
    touch($dir.'/a.slangp');
    touch($dir.'/crt/royale.slangp');
    touch($dir.'/readme.txt');       // not a preset
    touch($dir.'/pass.slang');       // a shader pass, not a preset

    expect(Shaders::in($dir))->toBe([
        $dir.'/a.slangp',
        $dir.'/b.slangp',
        $dir.'/crt/royale.slangp',
    ]);

    array_map('unlink', [
        $dir.'/a.slangp', $dir.'/b.slangp', $dir.'/crt/royale.slangp',
        $dir.'/readme.txt', $dir.'/pass.slang',
    ]);
    rmdir($dir.'/crt');
    rmdir($dir);
});

it('returns an empty list for a missing directory', function () {
    expect(Shaders::in('/no/such/shader/dir'))->toBe([]);
});

it('sends the preset path to the bridge and stays fluent', function () {
    $GLOBALS['__nativephp_mock']['Emulator.SetShader'] = '{"status":"applied"}';
    $emu = Emulator::surface('main');
    expect($emu->setShader('/sd/crt-royale.slangp'))->toBe($emu);

    $call = end($GLOBALS['__nativephp_calls']);
    expect($call['function'])->toBe('Emulator.SetShader');
    expect($call['payload']['path'])->toBe('/sd/crt-royale.slangp');
});

it('passes null to clear the shader', function () {
    $GLOBALS['__nativephp_mock']['Emulator.SetShader'] = '{"status":"cleared"}';
    Emulator::surface('main')->setShader(null);

    expect(end($GLOBALS['__nativephp_calls'])['payload']['path'])->toBeNull();
});

it('treats SHADER_FAILED as an operational (non-throwing) code', function () {
    expect(EmulatorErrorCode::ShaderFailed->throwsAsException())->toBeFalse();
    expect(EmulatorErrorCode::ShaderFailed->value)->toBe('SHADER_FAILED');
});
