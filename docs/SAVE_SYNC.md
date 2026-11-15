# Save sync through folder-sync apps

Status: built 2026-11-14 for Windows (game and launcher) and Android (the
game's folder argument). Code: `src/save_sync.{h,cpp}`, the PROFILE > SAVE SYNC
page in `src/ui/settings_menu.cpp`, `launcher/…/MainWindow.Sync.cs` and
`SaveSyncStatus.cs`, and `syncFolder()` in `LavenderShiftActivity.java`.
Test: `tools/save_sync_test.cpp` (below).

**Goal.** A player who already runs a folder-sync app (Syncthing, Syncthing-Fork
on Android, Dropbox, OneDrive, Google Drive for desktop, Nextcloud, a NAS share,
or a USB stick) points Lavender Shift at a folder that app syncs. The save and
achievements then follow them between the PC and the Android device. There's
no manual copying, no server of ours, and no way for the sync app to corrupt a
save.

**Non-goals.** Our own cloud service or account. Live sync while two devices
play at once. Syncing caches, shaders, logs or graphics settings.

## Decisions

1. **Syncthing is the main target**, but nothing depends on it. The design only
   needs a folder that eventually looks the same on every device. The
   Android default folder was chosen so that Syncthing-Fork can reach it.
2. **The "which save" choice is made in the game only**, on PROFILE > SAVE SYNC,
   so the PC and Android share one implementation. The launcher shows a status
   line and doesn't decide anything.
3. **`marketplace\tmpfile` stays in the save.** It's 15 MB and mostly zeros, and
   we can't show that the title recreates it, so leaving it out risks the save.
   The archive is a *stored* (uncompressed) ZIP. FH2's save files keep their
   size between saves, so each file stays at the same offset in the archive, and
   Syncthing (which sends only changed 128 KiB blocks) transfers just what the
   title rewrote. A deflated archive would change throughout on every save.
4. **Lineage decides, clocks never do.** Timestamps are shown to the player and
   nothing more.

## Why syncing `user\` as it is doesn't work

- `user\` mixes the save with things that must not travel: `cache\` (large and
  tied to the GPU), `logs\`, `backups\`, `drivers\` (Android) and
  `config\lavender_shift.toml`, whose values are per device.
- The save is several files that must agree with each other. A file-by-file
  sync can deliver a new `PlayerDatabase` next to an old `ForzaProfile`, and
  when both devices played, sync apps resolve the conflict file by file.
- The title writes those files in place while it runs.
- On Android the save is in `Android/data/<package>`, which no other app can
  open on Android 11 and later.

## How it works

The live save stays where it is. Each device writes **only its own files** in
`<sync folder>\devices\`, so a sync app never sees two writers of one file:

```
<sync folder>\devices\
  <device-id>.lsave              stored ZIP of the profile folders (not 0000000000000000)
  <device-id>.achievements.toml  copy of user\achievements\4D530AA4.toml
  <device-id>.toml               manifest, written last
```

Manifest (flat TOML, the same `name = value` rules as the settings file):

```toml
format = 1
device_id = "8c1e…"          # 32 hex digits, random, made once per device
device_name = "MULTIVAC"     # the computer's name, Build.MODEL on Android, or --lavender_device_name
platform = "windows"
written_utc = "2026-11-14T18:02:11Z"
save_hash = "…"              # SHA-256 over the sorted save file names, sizes and contents
based_on = "…"               # the newest entry of history
history = "h1,h2,…"          # up to 64 earlier or replaced save hashes, newest first
archive_sha256 = "…"
archive_bytes = 22806528
achievements = 23
```

A device's state lives in `user\config\save_sync.toml`: its id, the machine
name the id belongs to, `current` (the hash of the save it has), `history`,
the last export, and a pending import. If the machine name changes (a copied or
cloned user folder), the device gets a new id, so two installs never write the
same files.

**Reading a remote.** A remote is any `<32 hex>.toml` other than this device's.
Its archive must have the size and SHA-256 the manifest gives, which covers a
manifest that arrives before its archive, or a partial download. Sync-app
conflict copies (`*.sync-conflict-*`, `(conflicted copy)`) are never read,
because their names aren't device ids. A manifest with a newer `format` is
skipped with a log line.

**Relation** of a remote R to this device:

| Relation | Condition |
|---|---|
| Same | `R.save_hash == current` |
| Local ahead | R's hash is in this device's history |
| Remote ahead | this device has no save, or `current` is in R's history |
| Diverged | neither |

### At start (`OnConfigurePaths`, before the title or the achievements load)

1. Hash the live save. If it differs from `current`, that's progress made since
   the last export (a crash, or sync was off), so the save advances (the old
   hash goes into history).
2. Merge every remote's achievement unlocks into this device's (the union,
   keeping the earliest time).
3. If the player chose a save on SAVE SYNC, import it. Otherwise, if exactly
   one newest remote is *remote ahead*, import it without asking.
4. **Import** extracts the archive to staging, checks its hash, adds it as a
   backup slot (`…-sync`), and puts it in place with the save backups' restore.
   The restore first backs up the current save (`…-before-restore`), so every
   import can be undone from SAVE BACKUPS. The remote's history is merged into
   this device's.
5. Publish this device's save if it changed.

### While the game runs

- **Export** runs on the save backups thread right after each snapshot, from
  the snapshot's copy and never from the live files. That's at most once per
  ten minutes, after the save has been still for five seconds, and once at
  exit. After the title stops, the game exports from the live folder once more
  (this also covers backups being off).
- **Nothing is imported mid-session.** A watcher polls the folder 15 s after
  start and then every 30 s, and shows toasts: "LOADED THE SAVE FROM X",
  "A NEWER SAVE FROM X IS WAITING" (it loads at the next start), "YOUR SAVE ALSO
  CHANGED ON X" (choose on SAVE SYNC), or "THE SYNC FOLDER IS NOT REACHABLE".

### Choosing (SETTINGS > PROFILE > SAVE SYNC)

Lists each other device with *same save*, *older*, *newer* or *different* and
the time it last shared. A newer or different save offers:

- **Load X's save at next start**: imported at the next start, like a backup
  restore.
- **Keep this device's save**: X's save hash goes into this device's history,
  and the next export publishes it. X then sees this device as *remote ahead*
  and loads it without asking. Neither device asks again.

## Turning it on

- **PC launcher**: **Save sync** in the footer opens a menu to choose a folder,
  open it, or turn sync off. A folder inside the install is refused. The choice
  is saved in `launcher.json` as `SyncFolder`, and Play passes it through
  `launch.ps1 -GameArguments --lavender_sync_folder=<folder>`.
  `launch.ps1` now quotes arguments that contain spaces, such as folders under
  `Google Drive` or `OneDrive - …`. The line under Play shows the status from
  the manifests (`SaveSyncStatus.cs`).
- **Without the launcher**: `--lavender_sync_folder=<folder>` (and
  optionally `--lavender_device_name=<name>`) on the command line, or the
  same names in `user\config\lavender_shift.toml`. The game reads them from the
  file itself, because sync starts before the SDK loads that file.
- **Android**: always on, in `Android/media/studio.deimos.lavendershift/sync`
  on internal storage. Point Syncthing-Fork or BasicSync (with "All files
  access") at that folder. A `--lavender_sync_folder=` line in
  `user/config/android-arguments.txt` overrides the folder, and an empty value
  turns sync off.
- A missing folder (an unplugged drive) is reported, never created. Only the
  `devices` subfolder is made inside an existing folder.

## Test

```
cmake --build --preset win-amd64-release --target lavender_shift_save_sync_test
out\build\win-amd64-release\lavender_shift_save_sync_test.exe
```

The test uses two user folders and one sync folder, and covers:

- the archive round trip, and a hash that ignores caches and DLC;
- the first device's save loading on a second device that has none;
- fast-forward, with the replaced save kept as a backup;
- divergence seen from both sides;
- achievement merging;
- *keep this device's*, after which the other device loads it without asking;
- a chosen import;
- a damaged archive ignored, then loaded once it's whole;
- a sync-conflict copy ignored;
- a missing folder reported and not created.

## Not done yet

- **Checked on an Odin 2 Portal, 2026-11-14,** with BasicSync (a Syncthing
  wrapper) on the Odin and Syncthing in Docker on multivac:
  - BasicSync reads and writes `Android/media/studio.deimos.lavendershift/sync`,
    and files it writes there are owned by the game's app user, so the game
    reads them. Both directions synced.
  - A throwaway user folder with no save listed MULTIVAC and the Odin's own
    save as *newer* and loaded neither on its own. Choosing MULTIVAC's save
    imported it at the next start (`sync.import`, `save.restore.applied`),
    and `PlayerDatabase` matched multivac's byte for byte.
  - Syncthing in Docker on Windows gets no file-change events through the bind
    mount, so it only notices a new export at its next full rescan (an hour
    by default). Set the folder's rescan interval to a minute or so.
  - The APK takes `classes.dex` from the prebuilt shell, which doesn't have
    the `syncFolder()` change until the shell is rebuilt. The test used a dex
    compiled from this checkout and copied into the shell cache.
- **Checked on multivac, 2026-11-14,** with two copies of the user folder
  (TEST-A and TEST-B) and one folder whose name has a space:
  - A published at start, and Python's `zipfile` read the archive as valid;
  - B, with a changed save, showed the "also changed" toast, and the SAVE SYNC
    page listed TEST-A as *different*;
  - choosing *load* set the pending import, and the next start backed up B's
    save, put A's in place and republished it.

  Not yet checked: an export during play (it uses the backup snapshot path),
  and the launcher's Save sync menu and status line on screen.
- **The PC → Android "Save" share** (`SetupActivity`) still copies the save on
  its own. The two features agree anyway: at the next start the device treats
  the copied save as its new save (its old one goes into history), which is the
  same save the PC published, so the device sees the PC as *same*.
- Photos (an append-only export) and portable settings (cheats, frame rate).
