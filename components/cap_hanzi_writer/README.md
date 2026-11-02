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

`CONFIG_HANZI_WRITER_ENABLE` defaults on with Mosaic UI. It uses the product's
existing LVGL 9.5 Canvas and display service, without enabling LVGL vector, SVG
or ThorVG support. It neither initializes another LVGL runtime nor accesses the
panel directly. No dependency on the sibling desktop project remains in the build.

## Tools and behavior

- `hanzi_writer_show({"character":"饕"})`: validate one UTF-8 Han ideograph,
  fetch its JSON and play the animation once at 1.5x speed, leaving the finished
  glyph visible. The tool only takes the character.
- `hanzi_writer_stop({})`: close this component's view and restore the baseline
  Claw UI. It succeeds when no Hanzi view is open and never closes another app.

The round-screen view contains only the centered animation, with no title,
status text, buttons or home indicator. Its canvas occupies 70% of the screen
diameter (336 pixels on a 480-pixel display). When playback finishes the timer
and rendering resources are freed; tapping anywhere then closes the view.
Taps during playback are ignored.
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

Registration creates only a lifecycle mutex. The first show request creates a
worker and command queue; they exist while requests or a Hanzi view remain.
After closing the view (or a failed show), the worker drains accepted commands,
deletes the queue and exits with `vTaskDeleteWithCaps`, releasing its PSRAM stack.
Stopping an idle tool does not start a worker. Tool callers wait for show/stop
completion; group stop uses the same close path.

The lifecycle mutex protects queue creation, enqueue and deletion. It is never
held across display or LVGL operations. LVGL owns animation/touch callbacks under
the existing adapter lock. Close callbacks enqueue a generation-tagged request
instead of closing from inside LVGL. Cleanup runs after the display service
pauses LVGL. Canvas objects are deleted before their backing draw buffers.
The worker uses a 32 KiB PSRAM stack for TLS and initial path rasterization.
Its LVGL session uses the service's 8 KiB PSRAM stack, since animation frames
no longer invoke ThorVG. The worker uses `xTaskCreateWithCaps`; the adapter uses
its existing PSRAM-stack option.
Download logs print the
actual URL, and tool errors distinguish downloading from opening or loading the
LVGL display session. Mosaic's screen-power lock serializes sleep/wake with presenter
handoffs and wakes the panel before an exclusive producer starts.

The renderer reads the data package's absolute `M/L/Q/C/Z` paths directly;
it does not construct or parse SVG XML documents. Quadratic and cubic curves
are flattened to within 0.2 display pixels. A nonzero-winding scanline fill
uses four vertical samples with horizontal pixel coverage to prepare each
cropped A8 stroke mask. The original coordinate transform, contours and
median paths are retained.

The round-capped brush is the union of capsules along the median, including
the existing 100-unit extensions at each end. It writes antialiased coverage
into one A8 scratch buffer sized to the largest stroke mask. There is no
full-canvas ARGB scratch image, hidden scratch Canvas or per-frame allocation.
The new rasterizer can produce slightly different edge coverage than ThorVG;
the visual result requires manual acceptance.

The renderer caches the grid, guide and completed strokes in one RGB565 buffer,
rebuilding it only when a stroke completes or guide visibility changes. Each
animation frame copies that base and composites the active stroke. On the
336-pixel canvas this buffer costs about 221 KiB. Once playback finishes, the
renderer, stroke masks, brush data and parsed character are freed. This releases
the 221 KiB base buffer and variable-sized A8 brush buffer plus the per-character data, retaining
only the finished canvas and display session until the user closes it.
The original stroke contours and median-driven reveal are preserved.

The firmware enables PSRAM XIP, so linked code and read-only data also consume
PSRAM at boot. The lifecycle changes above release dynamic resources; they do
not unload statically linked code or change global XIP settings. Removing this
component's vector/SVG/ThorVG dependency reduces its boot-time XIP footprint;
the rest of LVGL remains available to existing applications.

## Source and data attribution

The portable data parser/player and contour-mask/median-brush approach were
ported from the user's `hanzi-writer` desktop prototype (`src/hanzi` and
`src/app/hanzi_render.*`). The device's path rasterizer and capsule brush avoid
the prototype's SVG/ThorVG backend. Characters still use the original stroke
outlines, rather than a font or a centerline-only approximation.

Character data comes from [hanzi-writer-data](https://github.com/chanind/hanzi-writer-data),
derived from Make Me a Hanzi. Its separate Arphic Public License is preserved
in `ARPHICPL.TXT`; it is not covered by the firmware's Apache license.

## Validation scope

Firmware compilation can be checked on the host.
The final animation appearance, tap-to-close behavior, round-screen placement
and display handoff require manual device acceptance. Tool description examples
specify expected extraction but do not prove the configured LLM will always
choose correctly.
