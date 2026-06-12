# KevinBatdorf\RetroEmulator Plugin for NativePHP Mobile

A NativePHP Mobile plugin

## Installation

```bash
composer require kevinbatdorf/retro-emulator
```

## Usage

```php
use Kevinbatdorf\RetroEmulator\Facades\KevinBatdorf\RetroEmulator;

// Execute functionality
$result = KevinBatdorf\RetroEmulator::execute(['option1' => 'value']);

// Get status
$status = KevinBatdorf\RetroEmulator::getStatus();
```

## Listening for Events

```php
use Livewire\Attributes\On;

#[On('native:Kevinbatdorf\RetroEmulator\Events\KevinBatdorf\RetroEmulatorCompleted')]
public function handleKevinBatdorf\RetroEmulatorCompleted($result, $id = null)
{
    // Handle the event
}
```

## License

MIT