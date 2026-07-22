# Note Editor Spike (BLE Keyboard)

Experimental Markdown note editor for writing on the device with a Bluetooth
keyboard. All of it is compiled out of normal builds: it exists only in the
`spike` PlatformIO environment, which defines `EDITOR_BLE_SPIKE=1` and
`FREEINK_CAP_BLE_HID_HOST=1` and links NimBLE-Arduino.

## Building

```
pio run -e spike
```

Note: the spike build carries the NimBLE BLE stack, which costs roughly 50KB
of RAM headroom compared to the default build (static data plus IRAM). The
`CONFIG_BT_NIMBLE_*` build flags cannot shrink this: arduino-esp32 ships a
prebuilt `sdkconfig.h` whose defines win over the command line. Reducing it
would require rebuilding the framework libs with a custom sdkconfig.

## Using it

- **Notes** item on the Home menu opens a note picker (reuses the file browser
  in a pick-note mode): lists `.md`/`.txt` files under `/notes` plus a
  synthetic "New note" row.
- The editor renders Markdown as you type: `#`/`##`/`###` headings (bold, H1
  underlined), `**bold**`, `*italic*`.
- **Settings > Bluetooth** scans for BLE HID keyboards and pairs/forgets them.
  Bonds persist in NVS across flashes and reboots.

### Keys

| Input | Action |
|---|---|
| Ctrl+S (keyboard) | Save to `/notes/<title>.md` |
| Back (device) | Save and exit to Home |
| Confirm (device) | Rotate the screen (editor-local) |
| Left / Right (device) | Font smaller / bigger |

## Design notes

- The editor's 16KB text buffer and 4KB line index are heap-allocated on
  entry and freed on exit, so closed-editor RAM cost is near zero.
- The editor frees the SD font caches before starting NimBLE: BLE init needs
  a large contiguous allocation that a fully-loaded Home heap cannot provide.
- The button legend is drawn on the physical bottom edge (where the buttons
  are); the editor reserves that edge in whatever logical orientation is
  active so text never renders under it.
- Editor core (`src/editor/`) is plain C++ with no Arduino dependencies and
  is covered by host-side unit tests (`test/text_editor`, `test/markdown_parser`,
  `test/note_names`).
