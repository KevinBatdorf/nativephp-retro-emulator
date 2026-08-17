/**
 * Retro Emulator for NativePHP Mobile — JavaScript bridge.
 *
 * One named export per bridge function, for SPA frontends (Inertia +
 * Vue/React) that call native functions without Livewire. Params pass
 * to the native layer verbatim.
 *
 * @example
 * import Emulator, { LoadRom, onNativeEvent } from '@kevinbatdorf/retro-emulator';
 *
 * await Emulator.Boot({ system: 'snes' });
 * await LoadRom({ path: '/roms/game.sfc' });
 *
 * const off = onNativeEvent('EmulatorStarted', (payload) => console.log(payload));
 * off(); // unsubscribe
 */

const baseUrl = '/_native/api/call';

async function bridgeCall(method, params = {}) {
    const response = await fetch(baseUrl, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'X-CSRF-TOKEN': document.querySelector('meta[name="csrf-token"]')?.content || ''
        },
        body: JSON.stringify({ method, params })
    });

    if (!response.ok) {
        throw new Error(`Native call failed with status ${response.status}`);
    }

    const result = await response.json();

    if (result.status === 'error') {
        throw new Error(result.message || 'Native call failed');
    }

    return result.data;
}

export async function Boot(params = {}) {
    return bridgeCall('Emulator.Boot', params);
}

export async function LoadSystem(params = {}) {
    return bridgeCall('Emulator.LoadSystem', params);
}

export async function LoadRom(params = {}) {
    return bridgeCall('Emulator.LoadRom', params);
}

export async function StageSlot(params = {}) {
    return bridgeCall('Emulator.StageSlot', params);
}

export async function Pause(params = {}) {
    return bridgeCall('Emulator.Pause', params);
}

export async function Resume(params = {}) {
    return bridgeCall('Emulator.Resume', params);
}

export async function Stop(params = {}) {
    return bridgeCall('Emulator.Stop', params);
}

export async function StateSave(params = {}) {
    return bridgeCall('Emulator.StateSave', params);
}

export async function StateLoad(params = {}) {
    return bridgeCall('Emulator.StateLoad', params);
}

export async function UndoStateSave(params = {}) {
    return bridgeCall('Emulator.UndoStateSave', params);
}

export async function UndoStateLoad(params = {}) {
    return bridgeCall('Emulator.UndoStateLoad', params);
}

export async function ReadMemory(params = {}) {
    return bridgeCall('Emulator.ReadMemory', params);
}

export async function ReadMemoryAsync(params = {}) {
    return bridgeCall('Emulator.ReadMemoryAsync', params);
}

export async function WriteMemory(params = {}) {
    return bridgeCall('Emulator.WriteMemory', params);
}

export async function WatchMemory(params = {}) {
    return bridgeCall('Emulator.WatchMemory', params);
}

export async function UnwatchMemory(params = {}) {
    return bridgeCall('Emulator.UnwatchMemory', params);
}

export async function ClearMemoryWatches(params = {}) {
    return bridgeCall('Emulator.ClearMemoryWatches', params);
}

export async function SetAudio(params = {}) {
    return bridgeCall('Emulator.SetAudio', params);
}

export async function SetVideo(params = {}) {
    return bridgeCall('Emulator.SetVideo', params);
}

export async function Configure(params = {}) {
    return bridgeCall('Emulator.Configure', params);
}

export async function PickRom(params = {}) {
    return bridgeCall('Emulator.PickRom', params);
}

export async function Rewind(params = {}) {
    return bridgeCall('Emulator.Rewind', params);
}

export async function ToggleRewind(params = {}) {
    return bridgeCall('Emulator.ToggleRewind', params);
}

export async function SetSystemOptions(params = {}) {
    return bridgeCall('Emulator.SetSystemOptions', params);
}

export async function FastForward(params = {}) {
    return bridgeCall('Emulator.FastForward', params);
}

export async function SetInputMapping(params = {}) {
    return bridgeCall('Emulator.SetInputMapping', params);
}

export async function ConnectDevice(params = {}) {
    return bridgeCall('Emulator.ConnectDevice', params);
}

export async function SetAxis(params = {}) {
    return bridgeCall('Emulator.SetAxis', params);
}

export async function AimAt(params = {}) {
    return bridgeCall('Emulator.AimAt', params);
}

export async function SetRumble(params = {}) {
    return bridgeCall('Emulator.SetRumble', params);
}

export async function SetShader(params = {}) {
    return bridgeCall('Emulator.SetShader', params);
}

export async function AddCheat(params = {}) {
    return bridgeCall('Emulator.AddCheat', params);
}

export async function RemoveCheat(params = {}) {
    return bridgeCall('Emulator.RemoveCheat', params);
}

export async function ClearCheats(params = {}) {
    return bridgeCall('Emulator.ClearCheats', params);
}

export async function PressButton(params = {}) {
    return bridgeCall('Emulator.PressButton', params);
}

export async function ReleaseButton(params = {}) {
    return bridgeCall('Emulator.ReleaseButton', params);
}

export async function SetButtons(params = {}) {
    return bridgeCall('Emulator.SetButtons', params);
}

export async function Screenshot(params = {}) {
    return bridgeCall('Emulator.Screenshot', params);
}

export async function GetStatus(params = {}) {
    return bridgeCall('Emulator.GetStatus', params);
}

export async function GetRegion(params = {}) {
    return bridgeCall('Emulator.GetRegion', params);
}

export async function GetPorts(params = {}) {
    return bridgeCall('Emulator.GetPorts', params);
}

export async function GetSystems(params = {}) {
    return bridgeCall('Emulator.GetSystems', params);
}

export async function GetEngineOptions(params = {}) {
    return bridgeCall('Emulator.GetEngineOptions', params);
}

export async function GetPressedButtons(params = {}) {
    return bridgeCall('Emulator.GetPressedButtons', params);
}

export async function GetInputDevices(params = {}) {
    return bridgeCall('Emulator.GetInputDevices', params);
}

export async function WindowMetrics(params = {}) {
    return bridgeCall('Emulator.WindowMetrics', params);
}
/**
 * `name` is the short event class ("EmulatorStarted") or the fully-qualified
 * "KevinBatdorf\\RetroEmulator\\Events\\EmulatorStarted". Returns an unsubscribe.
 */
export function onNativeEvent(name, handler) {
    const listener = (e) => {
        const event = e.detail?.event ?? '';
        if (event !== name && !event.endsWith('\\' + name)) return;
        handler(e.detail?.payload, event);
    };
    document.addEventListener('native-event', listener);
    return () => document.removeEventListener('native-event', listener);
}

export const Emulator = {
    Boot,
    LoadSystem,
    LoadRom,
    StageSlot,
    Pause,
    Resume,
    Stop,
    StateSave,
    StateLoad,
    UndoStateSave,
    UndoStateLoad,
    ReadMemory,
    ReadMemoryAsync,
    WriteMemory,
    WatchMemory,
    UnwatchMemory,
    ClearMemoryWatches,
    SetAudio,
    SetVideo,
    Configure,
    PickRom,
    Rewind,
    ToggleRewind,
    SetSystemOptions,
    FastForward,
    SetInputMapping,
    ConnectDevice,
    SetAxis,
    AimAt,
    SetRumble,
    SetShader,
    AddCheat,
    RemoveCheat,
    ClearCheats,
    PressButton,
    ReleaseButton,
    SetButtons,
    Screenshot,
    GetStatus,
    GetRegion,
    GetPorts,
    GetSystems,
    GetEngineOptions,
    GetPressedButtons,
    GetInputDevices,
    WindowMetrics,
    onNativeEvent,
};

export default Emulator;
