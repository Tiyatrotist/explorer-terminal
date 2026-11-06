# Changelog

## [0.3.2] - 2026-11-06
### Fixed
- The diagnostic log no longer records which keys were typed, only that a key was handled.

## [0.3.1] - 2026-11-06
### Added
- Diagnostic log lines for activation, clicks and key handling in `%TEMP%\explorer-terminal.log`.

## [0.3.0] - 2026-11-06
### Changed
- Typing no longer depends on keyboard focus, which Windows refuses to give to the embedded console. While a terminal is active, its keys are taken by the keyboard hook and written into the console's input buffer.
- The title strip shows whether the terminal is active ("typing here").

## [0.2.2] - 2026-11-06
### Fixed
- The folder was not followed after the terminal opened: a thread timer's id was compared with the wrong value, so the 500 ms tick never ran.

## [0.2.1] - 2026-11-05
### Changed
- The default look takes the color scheme and font of the Windows Terminal profile.

## [0.2.0] - 2026-11-05
### Added
- Title strip with drag-to-resize and a close button; the height is remembered.
- Look setting (Windows Terminal, console Properties, plain).

## [0.1.0] - 2026-11-05
### Added
- First version: a console docked at the bottom of a File Explorer window, a hotkey, and following the Explorer folder with PowerShell.
