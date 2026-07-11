<?php

namespace KevinBatdorf\RetroEmulator;

enum Status: string
{
    case Stopped = 'stopped';
    case Loading = 'loading';
    case Running = 'running';
    case Paused = 'paused';
}
