# Hanzi Writer capability

Native Claw tools for showing one Chinese character's stroke-order animation on
the ESP-Mosaico display. The `hanzi_writer_show` tool description instructs the
LLM how to extract the target glyph directly from natural-language questions such
as “永怎么写” and “饕餮的饕怎么写”.

## Integration

`main` registers the `cap_hanzi_writer` external group before `app_claw_start()`.
The group participates in the existing capability enable/visibility settings and
is visible to the LLM by default (`llm_visible_by_default = true`) so it can be
invoked in a single tool-call turn without a separate Skill activation round-trip.
No changes to the ESP-Claw submodule are required.

`CONFIG_HANZI_WRITER_ENABLE` defaults on with Mosaic UI and selects LVGL vector,
SVG and ThorVG support. It uses the product's existing LVGL 9.5 and display
service; it neither initializes another LVGL runtime nor accesses the panel
directly. No dependency on the sibling desktop project remains in the build.

## Tools and behavior

- `hanzi_writer_show({"character":"饕"})`: validate one UTF-8 Han ideograph,
  fetch its JSON and play the animation once at 1.5x speed, leaving the finished
  glyph visible. The tool only takes the character.
- `hanzi_writer_stop({})`: close this component's view and restore the baseline
  Claw UI. It succeeds when no Hanzi view is open and never closes another app.

The round-screen view contains only the centered animation, with no title,
status text, buttons or home indicator. Its canvas occupies 70% of the screen
diameter (336 pixels on a 480-pixel display). When playback finishes the timer
pauses; tapping anywhere then closes the view. Taps during playback are ignored.
The existing bottom swipe exit gesture and physical Back action remain available.
Replacing a Hanzi view is supported;
an exclusive display session belonging to another component is reported as busy.

The tool returns `started` only after data loading and UI initialization succeed,
not after the animation finishes. Network and initialization errors are returned
to the Agent. The tool description instructs it to report actual failures and ask
for clarification when the target glyph is ambiguous. Character extraction is
model reasoning guided by the tool description; this component does not parse
whole questions.

## Data and ownership

Each show request reads one JSON document from
`https://unpkg.byted-static.com/hanzi-writer-data/2.0.1/<encoded-character>.json`.
The UTF-8 filename is percent-encoded. The configurable HTTPS base is
`CONFIG_HANZI_WRITER_DATA_BASE_URL`. No corpus is downloaded at build time,
bundled into flash or persisted on the device. Network access to the data CDN is
required even for a repeated character. Responses with a Content-Length allocate
that size; chunked responses start at 4 KiB and grow as needed, up to 128 KiB.
Documents remain limited to 128 KiB;
each HTTP operation has a 15-second timeout and the read loop has a 30-second
overall deadline. Unsupported glyphs return a missing-data error.

One process-lifetime worker owns all network/display operations. Tool callers
submit bounded queue commands and wait for initialization or stop completion;
the worker remains allocated while the capability is disabled, and group stop
closes its current display session. LVGL owns animation/touch callbacks under
the existing adapter lock. Close callbacks enqueue a generation-tagged request
instead of closing from inside LVGL. Cleanup runs after the display service
pauses LVGL and releases this view's timer, objects, stroke masks and buffers.
Canvas objects are deleted before their backing draw buffers. Both the Hanzi
worker and LVGL adapter use 32 KiB PSRAM stacks for synchronous ThorVG rendering.
The worker uses `xTaskCreateWithCaps`; the adapter uses its existing PSRAM-stack
option. Download logs print the
actual URL, and tool errors distinguish downloading from opening or loading the
LVGL display session. Mosaic's screen-power lock serializes sleep/wake with presenter
handoffs and wakes the panel before an exclusive producer starts.

The renderer caches the grid, guide and completed strokes in one RGB565 buffer,
rebuilding it only when a stroke completes or guide visibility changes. Each
animation frame copies that base and composites the active stroke. On the
336-pixel canvas this buffer costs about 221 KiB and is released with the view;
SVG masks and the median-driven brush remain unchanged.

## Source and data attribution

The portable parser/player and SVG mask renderer were ported from the user's
`hanzi-writer` desktop prototype (`src/hanzi` and `src/app/hanzi_render.*`).
The device view retains its median-driven brush clipped to exact SVG stroke
masks, rather than approximating the character using a font or centerlines.

Character data comes from [hanzi-writer-data](https://github.com/chanind/hanzi-writer-data),
derived from Make Me a Hanzi. Its separate Arphic Public License is preserved
in `ARPHICPL.TXT`; it is not covered by the firmware's Apache license.

## Validation scope

Firmware compilation can be checked on the host.
The final animation appearance, tap-to-close behavior, round-screen placement
and display handoff require manual device acceptance. Tool description examples
specify expected extraction but do not prove the configured LLM will always
choose correctly.
