## kevinbatdorf/retro-emulator

A NativePHP Mobile plugin

### Installation

```bash
composer require kevinbatdorf/retro-emulator
```

### PHP Usage (Livewire/Blade)

Use the `KevinBatdorf\RetroEmulator` facade:

@verbatim
<code-snippet name="Using KevinBatdorf\RetroEmulator Facade" lang="php">
use Kevinbatdorf\RetroEmulator\Facades\KevinBatdorf\RetroEmulator;

// Execute the plugin functionality
$result = KevinBatdorf\RetroEmulator::execute(['option1' => 'value']);

// Get the current status
$status = KevinBatdorf\RetroEmulator::getStatus();
</code-snippet>
@endverbatim

### Available Methods

- `KevinBatdorf\RetroEmulator::execute()`: Execute the plugin functionality
- `KevinBatdorf\RetroEmulator::getStatus()`: Get the current status

### Events

- `KevinBatdorf\RetroEmulatorCompleted`: Listen with `#[OnNative(KevinBatdorf\RetroEmulatorCompleted::class)]`

@verbatim
<code-snippet name="Listening for KevinBatdorf\RetroEmulator Events" lang="php">
use Native\Mobile\Attributes\OnNative;
use Kevinbatdorf\RetroEmulator\Events\KevinBatdorf\RetroEmulatorCompleted;

#[OnNative(KevinBatdorf\RetroEmulatorCompleted::class)]
public function handleKevinBatdorf\RetroEmulatorCompleted($result, $id = null)
{
    // Handle the event
}
</code-snippet>
@endverbatim

### JavaScript Usage (Vue/React/Inertia)

@verbatim
<code-snippet name="Using KevinBatdorf\RetroEmulator in JavaScript" lang="javascript">
import { kevinBatdorf\RetroEmulator } from '@kevinbatdorf/retro-emulator';

// Execute the plugin functionality
const result = await kevinBatdorf\RetroEmulator.execute({ option1: 'value' });

// Get the current status
const status = await kevinBatdorf\RetroEmulator.getStatus();
</code-snippet>
@endverbatim