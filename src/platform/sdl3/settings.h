// The host's own settings file, beside the saves: the options a player chose survive a restart, which the game's own save only
// does once a save has been written and loaded (the front end at boot has none). The values are the game's own - its four
// volume groups, the music's stereo and the screen's widescreen - applied at the sound processor's first reset (that is where
// the game puts its defaults) and written back as they change.
#pragma once

// The sound processor's first reset: put the file's options into the game, if there is a file
void HostSettingsApplyOnce();
// Once a second, from the mixer: write the file when the options have changed
void HostSettingsKeep();
