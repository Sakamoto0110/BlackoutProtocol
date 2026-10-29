#pragma once

// ============================================================================
// Resource identifiers
//
// Shared between the resource script (BlackoutProtocol.rc) and the code that
// looks resources up at runtime (BlackoutProtocol.cpp).
//
// Keep this header free of anything but #define lines. The Visual Studio
// resource editor parses it, and it is included by the resource compiler,
// which does not understand C++.
// ============================================================================


// ============================================================================
// Icons
//
// IMPORTANT:
//
//     Windows Explorer shows the icon with the NUMERICALLY LOWEST id as the
//     icon of the .exe itself.
//
//     IDI_APPICON is deliberately the lowest id in this file. Any icon added
//     later must use a HIGHER number, or the shell will silently start
//     showing that one on the executable instead.
// ============================================================================

#define IDI_APPICON     101
