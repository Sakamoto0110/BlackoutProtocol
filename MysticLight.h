#pragma once

// ============================================================================
// Mystic Light
//
// Turns the motherboard RGB off and back on through MSI's official SDK
// (MysticLight_SDK_x64.dll), loaded dynamically.
//
// ----------------------------------------------------------------------------
// WHY A DEDICATED THREAD
//
//     MLAPI calls talk to the Mystic Light Service and can block for
//     hundreds of milliseconds. MLAPI_Initialize in particular is
//     asynchronous and carries a timeout of its own.
//
//     Doing that on the window thread would stall the message loop, and with
//     it the WM_POWERBROADCAST -> WM_BLACKOUT_FORCE_OFF path, which is the
//     exact mechanism that keeps the monitors down.
//
//     So every function below is NON-BLOCKING: it posts a request to the
//     thread and returns.
//
// ----------------------------------------------------------------------------
// REQUIRES ADMINISTRATOR
//
//     Without elevation MLAPI_GetDeviceInfo fails with -2
//     ("MLAPI_Initialize is timeout") even though MLAPI_Initialize already
//     returned S_OK.
//
//     The project handles this with
//     UACExecutionLevel=RequireAdministrator in the vcxproj, which is why
//     launching now raises a UAC prompt.
//
// ----------------------------------------------------------------------------
// DEGRADATION
//
//     None of this is load-bearing. If the SDK is missing, or the service
//     does not answer, Start() returns false and the remaining functions
//     become silent no-ops. The monitor blackout keeps working untouched.
// ============================================================================

namespace MysticLight
{
    // Loads the SDK and starts the thread.
    //
    // Returns false if the SDK could not be loaded. The other functions stay
    // safe to call either way.
    //
    bool Start();


    // Restores the lighting, stops the thread and releases the SDK.
    //
    // A clean exit should not leave the machine altered. If the process is
    // killed outright this never runs, and the state file left on disk gets
    // picked up and restored by the next run instead.
    //
    void Stop();


    // Saves the current state to disk, then blacks out every zone except the
    // indicator one, which is left flashing.
    //
    // If saved state already exists the capture is skipped -- otherwise the
    // blacked-out state itself would be recorded as the state to restore,
    // losing the original for good.
    //
    void LightsOff();


    // Restores the saved state and discards the file.
    //
    // No-op when there is nothing saved.
    //
    void LightsOn();
}
