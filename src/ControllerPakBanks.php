<?php

namespace KevinBatdorf\RetroEmulator;

/**
 * N64 Controller Pak capacity as ares' wire strings — the stock 32KiB pak or
 * the oversized Datel carts some games can address.
 */
enum ControllerPakBanks: string
{
    case Default = '32KiB (Default)';
    case Datel1Meg = '128KiB (Datel 1Meg)';
    case Datel4Meg = '512KiB (Datel 4Meg)';
    case Maximum = '1984KiB (Maximum)';
}
