# Window Time Tracker

A lightweight C++ / Win32 app that tracks how much time you spend on each
active window (process + title) in the background, and logs everything to
a CSV file. Lives in the system tray — no visible window, no clutter.

## Features

- Polls the foreground window every 2 seconds.
- Whenever it changes (new program or new window title), closes the previous
  session and appends it to the CSV, then starts tracking the new one.
- Sessions shorter than 1 second are discarded, to filter out noise from
  quick alt-tabbing.
- Tray icon menu (right-click):
  - **Today's summary** — shows total time per program for the current day
    (double-clicking the tray icon does the same).
  - **Open log folder** — opens the CSV location in File Explorer.
  - **Exit** — closes the app (flushes the current session first).
- Single-instance guard via a named mutex.

## Where data is stored

```
%APPDATA%\WindowTimeTracker\activity_log.csv
```

Columns: `Date, StartTime, EndTime, DurationSeconds, ProcessName, WindowTitle`

Open it directly in Excel (or any spreadsheet tool) for pivot tables and charts.

## Building

### Option A — Visual Studio (recommended)

1. Open "Developer Command Prompt for VS" (or "x64 Native Tools Command Prompt").
2. Navigate to the project folder:
   ```
   cd WindowTimeTracker
   ```
3. Build:
   ```
   cl /EHsc /O2 main.cpp user32.lib shell32.lib gdi32.lib advapi32.lib /Fe:WindowTimeTracker.exe
   ```

### Option B — CMake + MSVC

```
cmake -S . -B build
cmake --build build --config Release
```
The executable will be at `build\Release\WindowTimeTracker.exe`.

### Option C — MinGW-w64

```
g++ -municode -O2 -o WindowTimeTracker.exe main.cpp -luser32 -lshell32 -lgdi32 -ladvapi32
```

## Running at startup

The app has no visible window, so it's meant to be launched at login:

1. Press `Win + R`, type `shell:startup`, hit Enter.
2. Copy a shortcut to `WindowTimeTracker.exe` into that folder.

For more control (delayed start, running without a full interactive logon,
etc.), you can also register it as a Task Scheduler task.

## Customization

- **Custom tray icon**: replace `LoadIconW(nullptr, IDI_APPLICATION)` in
  `AddTrayIcon()` with an icon loaded from a resource (`.ico` + `.rc` file).
- **Polling interval**: change `POLL_INTERVAL_MS` in `main.cpp`.
- **Minimum session length**: change `MIN_SESSION_SECS`.
- **Exclude a specific program**: add a check at the top of
  `PollActiveWindow()` before recording the session.

## Notes

- Administrator privileges are not required, except to read the process
  name of windows belonging to elevated processes (those will be logged as
  "Unknown" otherwise).
- Tested on Windows 10/11. Should work on any Win32-compatible target.

## License

Feel free to use, modify, and distribute. Consider adding an explicit
license file (MIT is a common, permissive choice) if you plan to publish
this publicly.
