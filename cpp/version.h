#pragma once
// Single source of truth for the app version (used by code and fmdv.rc).
#define FMDV_VERSION_STR  "1.2.2"
#define FMDV_VERSION_WSTR L"1.2.2"
#define FMDV_VERSION_RC   1,2,2,0

// First version with the in-app updater (Ctrl+U). Installing an older release
// than this strands the user with no in-app way back up, so the picker
// confirms before installing anything below it.
#define FMDV_UPDATER_MIN_WSTR L"1.1.0"
