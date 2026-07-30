<?php

namespace KevinBatdorf\RetroEmulator\Commands;

use Illuminate\Console\Command;
use Illuminate\Filesystem\Filesystem;
use Illuminate\Support\Facades\Http;
use ZipArchive;

/**
 * Download libretro cores from the buildbot into the app's drop-in dir
 * (resources/emulator-cores/android/<abi>/), where copy-assets ships them.
 * Cores are the core authors' work under their own licences — this command
 * prints each one's licence line and whether the plugin has verified it.
 */
class FetchCoreCommand extends Command
{
    protected $signature = 'retro-emulator:fetch-core
        {cores* : Core names as the buildbot publishes them (snes9x, fceumm, mesen, …)}
        {--abi=* : ABIs to fetch (default: arm64-v8a and x86_64)}';

    protected $description = 'Download libretro cores into resources/emulator-cores for bring-your-own engines';

    private const BUILDBOT = 'https://buildbot.libretro.com/nightly/android/latest';

    public function handle(Filesystem $files): int
    {
        $abis = $this->option('abi') ?: ['arm64-v8a', 'x86_64'];
        $failures = 0;

        foreach ($this->argument('cores') as $core) {
            foreach ($abis as $abi) {
                $failures += $this->fetch($files, $core, $abi) ? 0 : 1;
            }

            $this->components->info(
                "{$core}: ".CopyAssetsCommand::coreLicenceNote("{$core}.so")
                .' — cores outside the README\'s verified table are supported but untested.'
            );
        }

        if ($failures === 0) {
            $this->line('Select a core per boot with e.g. new SfcConfig(backend: \'snes9x\').');
        }

        return $failures === 0 ? self::SUCCESS : self::FAILURE;
    }

    private function fetch(Filesystem $files, string $core, string $abi): bool
    {
        $name = "{$core}_libretro_android.so";
        $url = self::BUILDBOT."/{$abi}/{$name}.zip";
        $destination = resource_path("emulator-cores/android/{$abi}");

        $response = Http::timeout(120)->get($url);

        if (! $response->successful()) {
            $this->components->error(
                "{$core} ({$abi}): {$response->status()} from {$url} — check the name against "
                .'https://buildbot.libretro.com/nightly/android/latest/'
            );

            return false;
        }

        $files->ensureDirectoryExists($destination);
        $zipPath = "{$destination}/{$name}.zip";
        $files->put($zipPath, $response->body());

        try {
            $zip = new ZipArchive;

            if ($zip->open($zipPath) !== true || $zip->extractTo($destination) !== true) {
                $this->components->error("{$core} ({$abi}): could not unzip {$zipPath}");

                return false;
            }

            $zip->close();
        } finally {
            $files->delete($zipPath);
        }

        $this->components->twoColumnDetail(
            "{$core} ({$abi})",
            'resources/emulator-cores/android/'.$abi.'/'.$name,
        );

        return true;
    }
}
