<?php

namespace KevinBatdorf\RetroEmulator\Support;

/**
 * NativePHP's build runner discards hook output and continues past hook
 * failures — the injected preBuild check still stops a coreless build.
 */
class GradleGuard
{
    public const MARKER = '// NATIVEPHP_RETRO_EMULATOR_CORE_CHECK';

    public const ERROR_FILE = 'retro-emulator-hook-error.txt';

    public static function ensure(string $buildPath): void
    {
        $gradle = $buildPath.'/app/build.gradle.kts';

        if (! is_file($gradle)) {
            return;
        }

        $updated = self::inject((string) file_get_contents($gradle));

        if ($updated !== null) {
            file_put_contents($gradle, $updated);
        }
    }

    /** Null means the check is already present. */
    public static function inject(string $contents): ?string
    {
        if (str_contains($contents, self::MARKER)) {
            return null;
        }

        return rtrim($contents)."\n\n".self::snippet()."\n";
    }

    /** A stale error file would fail healthy builds — success must clear it. */
    public static function report(string $buildPath, ?string $error): void
    {
        $file = $buildPath.'/app/'.self::ERROR_FILE;

        if ($error === null) {
            if (is_file($file)) {
                @unlink($file);
            }

            return;
        }

        file_put_contents($file, $error."\n");
    }

    private static function snippet(): string
    {
        $errorFile = self::ERROR_FILE;

        return self::MARKER.<<<KOTLIN

tasks.matching { it.name == "preBuild" }.configureEach {
    doFirst {
        val hookError = layout.projectDirectory.file("{$errorFile}").asFile
        if (hookError.exists()) {
            throw GradleException("retro-emulator: " + hookError.readText().trim())
        }
        val staged = layout.projectDirectory.dir("src/main/jniLibs").asFile.listFiles()?.any { abi ->
            abi.listFiles()?.any { it.name.startsWith("libbackend_") && it.name.endsWith(".so") } == true
        } == true
        if (!staged) {
            throw GradleException(
                "retro-emulator: no emulator core libraries were staged into app/src/main/jniLibs, " +
                "so this APK would abort at launch. The plugin's copy-assets hook did not run — " +
                "re-run `php artisan native:run android`; the first build downloads the cores from " +
                "the plugin's GitHub release and needs network access to github.com."
            )
        }
    }
}
KOTLIN;
    }
}
