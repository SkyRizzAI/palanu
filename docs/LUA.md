# Lua on the device

`lua` is Lua 5.4 (ADR-0019). It runs scripts and has a prompt; a script is a process like any command, so it can be piped, run in
the background with `&`, and stopped with Ctrl-C.

## Running
- `lua` alone opens the prompt: an expression prints its value (`2^10` shows `1024.0`), a statement that is not finished continues on
  the next line (`>>`), Ctrl-D leaves.
- `lua FILE [ARGS]` runs a script; its arguments are in the table `arg` (`arg[0]` is the file). A first line starting with `#!` is
  skipped.
- `lua -e 'CODE'` runs one line.

## What is there
The standard libraries `string`, `table`, `math`, `utf8` and `coroutine`, and the base functions (`print`, `pairs`, `pcall`,
`load`, `dofile`, `loadfile`, ...). Lua's `io`, `os`, `package` (`require`) and `debug` libraries are not built: they work on the C
library's files, not the device's. In their place:

- `print(...)`, `io.write(...)`: to the terminal. `io.read()` reads a line (`io.read("n")` a number), nil at the end.
- `dofile(path)`, `loadfile(path)`: read the device's files.

## The pal module
| Function | What it does |
|---|---|
| `pal.readfile(path)` | the whole file as a string, or `nil, message` |
| `pal.writefile(path, text [, append])` | writes (or appends) the text; `true`, or `nil, message` |
| `pal.ls([path])` | a list of `{name=, dir=, size=}` |
| `pal.exists(path)`, `pal.remove(path)`, `pal.mkdir(path)` | as their names say |
| `pal.run(command)` | runs a shell command on this terminal and returns its exit status |
| `pal.sleep(ms)`, `pal.uptime()` | waits (Ctrl-C still stops it); milliseconds since boot |
| `pal.setting(name)` | a setting's value as text (`pal.setting("ui.theme")`), or nil |
| `pal.notify(title [, text])` | posts a notification |
| `pal.beep([ms])` | sounds the buzzer |
| `pal.tone(hz, ms)` | one tone for ms (0 Hz: a rest) |
| `pal.play(tune)` | an RTTTL tune or a built-in one (`"ode"`); `true`, or `nil, why` |
| `pal.version`, `pal.board` | the PalanuOS version and the board's name |

## Limits
A state may use 1 MB of memory (PSRAM first); past that Lua raises "not enough memory" and the script can recover with `pcall`. C
calls nest at most 64 deep (Lua functions calling Lua functions do not count). Precompiled chunks are refused: only text is loaded.

## Example
```lua
-- count the files in /home and log the result
local n = 0
for _, e in ipairs(pal.ls("/home")) do if not e.dir then n = n + 1 end end
print(("%d files in /home"):format(n))
pal.writefile("/home/count.log", pal.uptime() .. " " .. n .. "\n", true)
```
