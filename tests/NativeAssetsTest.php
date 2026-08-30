<?php

use Illuminate\Filesystem\Filesystem;
use KevinBatdorf\RetroEmulator\Support\NativeAssets;
use KevinBatdorf\RetroEmulator\Support\Podfile;

beforeEach(function () {
    $this->files = new Filesystem;
    $this->pluginPath = sys_get_temp_dir().'/retro-assets-'.uniqid();
    $this->files->ensureDirectoryExists($this->pluginPath.'/resources');
});

afterEach(function () {
    $this->files->deleteDirectory($this->pluginPath);
});

function writeManifest(string $pluginPath, array $overrides = []): array
{
    $manifest = array_replace_recursive([
        'release' => 'v9.9.9',
        'base_url' => 'https://example.com/releases/download',
        'assets' => [
            'android-jniLibs.zip' => [
                'platform' => 'android',
                'sha256' => str_repeat('0', 64),
                'extract_to' => 'resources/android/jniLibs',
                'probe' => 'resources/android/jniLibs/arm64-v8a/libbackend_ares.so',
            ],
            'RetroEmulator.xcframework.zip' => [
                'platform' => 'ios',
                'sha256' => str_repeat('0', 64),
                'extract_to' => 'build',
                'probe' => 'build/RetroEmulator.xcframework/Info.plist',
            ],
        ],
    ], $overrides);

    file_put_contents(
        $pluginPath.'/resources/native-assets.json',
        json_encode($manifest, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES)
    );

    return $manifest;
}

function makeZip(string $dir, array $entries): string
{
    $path = $dir.'/fixture-'.uniqid().'.zip';
    $zip = new ZipArchive;
    $zip->open($path, ZipArchive::CREATE);
    foreach ($entries as $name => $content) {
        $zip->addFromString($name, $content);
    }
    $zip->close();

    return $path;
}

describe('NativeAssets', function () {
    it('lists the platform assets whose probe file is absent', function () {
        writeManifest($this->pluginPath);

        expect(array_keys(NativeAssets::missing($this->pluginPath, 'android')))
            ->toBe(['android-jniLibs.zip'])
            ->and(array_keys(NativeAssets::missing($this->pluginPath, 'ios')))
            ->toBe(['RetroEmulator.xcframework.zip']);
    });

    it('reports nothing missing once the probes exist', function () {
        writeManifest($this->pluginPath);
        $probe = $this->pluginPath.'/resources/android/jniLibs/arm64-v8a/libbackend_ares.so';
        $this->files->ensureDirectoryExists(dirname($probe));
        file_put_contents($probe, 'lib');

        expect(NativeAssets::missing($this->pluginPath, 'android'))->toBe([]);
    });

    it('builds the release asset URL from the manifest', function () {
        $manifest = writeManifest($this->pluginPath);

        expect(NativeAssets::url($manifest, 'android-jniLibs.zip'))
            ->toBe('https://example.com/releases/download/v9.9.9/android-jniLibs.zip');
    });

    it('refuses a checksum mismatch and extracts nothing', function () {
        writeManifest($this->pluginPath);
        $zip = makeZip($this->pluginPath, ['arm64-v8a/libbackend_ares.so' => 'lib']);
        $spec = NativeAssets::manifest($this->pluginPath)['assets']['android-jniLibs.zip'];

        expect(NativeAssets::install($this->pluginPath, $spec, $zip))->toBeFalse()
            ->and(is_dir($this->pluginPath.'/resources/android/jniLibs'))->toBeFalse();
    });

    it('extracts a verified zip and confirms the probe', function () {
        $zip = makeZip($this->pluginPath, ['arm64-v8a/libbackend_ares.so' => 'lib']);
        writeManifest($this->pluginPath, [
            'assets' => ['android-jniLibs.zip' => ['sha256' => hash_file('sha256', $zip)]],
        ]);
        $spec = NativeAssets::manifest($this->pluginPath)['assets']['android-jniLibs.zip'];

        expect(NativeAssets::install($this->pluginPath, $spec, $zip))->toBeTrue()
            ->and(file_get_contents($this->pluginPath.'/resources/android/jniLibs/arm64-v8a/libbackend_ares.so'))
            ->toBe('lib');
    });

    it('fails install when the zip lacks the probe file', function () {
        $zip = makeZip($this->pluginPath, ['wrong-layout.txt' => 'nope']);
        writeManifest($this->pluginPath, [
            'assets' => ['android-jniLibs.zip' => ['sha256' => hash_file('sha256', $zip)]],
        ]);
        $spec = NativeAssets::manifest($this->pluginPath)['assets']['android-jniLibs.zip'];

        expect(NativeAssets::install($this->pluginPath, $spec, $zip))->toBeFalse();
    });
});

describe('Podfile', function () {
    $podfile = <<<'RUBY'
    platform :ios, '18.2'

    project 'NativePHP.xcodeproj'

    def shared_pods
      use_frameworks! :linkage => :static

      # NATIVEPHP_PLUGIN_PODS_START
      # NATIVEPHP_PLUGIN_PODS_END
    end

    target 'NativePHP' do
      shared_pods
    end
    RUBY;

    it('injects the pod after the plugin-pods markers', function () use ($podfile) {
        $result = Podfile::ensurePod($podfile, '../../vendor/kevinbatdorf/nativephp-retro-emulator');

        expect($result)->toContain(
            "# NATIVEPHP_PLUGIN_PODS_END\n  pod 'RetroEmulator', :path => '../../vendor/kevinbatdorf/nativephp-retro-emulator'"
        );
    });

    it('leaves an existing RetroEmulator pod untouched', function () use ($podfile) {
        $existing = str_replace(
            '# NATIVEPHP_PLUGIN_PODS_START',
            "# NATIVEPHP_PLUGIN_PODS_START\n  pod 'RetroEmulator', :path => '../../../nativephp-retro-emulator'",
            $podfile
        );

        expect(Podfile::ensurePod($existing, '../../vendor/kevinbatdorf/nativephp-retro-emulator'))->toBeNull();
    });

    it('is idempotent across runs', function () use ($podfile) {
        $once = Podfile::ensurePod($podfile, '../../vendor/x');

        expect(Podfile::ensurePod($once, '../../vendor/x'))->toBeNull();
    });

    it('returns null when no marker or shared_pods anchor exists', function () {
        expect(Podfile::ensurePod("target 'App' do\nend\n", '../../vendor/x'))->toBeNull();
    });

    it('computes the relative path between build and plugin dirs', function () {
        expect(Podfile::relativePath('/app/nativephp/ios', '/app/vendor/kevinbatdorf/nativephp-retro-emulator'))
            ->toBe('../../vendor/kevinbatdorf/nativephp-retro-emulator')
            ->and(Podfile::relativePath('/app/nativephp/ios', '/code/plugin'))
            ->toBe('../../../code/plugin');
    });
});
