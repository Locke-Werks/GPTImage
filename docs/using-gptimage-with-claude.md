# Using GPTImage from Claude

Operating instructions for a Claude instance that has the GPTImage MCP server
connected. Written for the agent, not the human: the wording is imperative on
purpose, so most of it can be pasted into a `CLAUDE.md` or project instructions
verbatim.

Every behavior below was checked against the server source and against a live
deployment, not inferred from tool names. `https://your-host` stands in for
wherever your server is reachable.

---

## Drop-in block

Paste this into `CLAUDE.md`. The rest of this document is the reference behind
it.

```markdown
## GPTImage

You have five tools. Four are the two OpenAI GPT Image 2.5 models crossed with
generate and edit; the fifth fetches a slow render.

- `gptimage_generate_flare` / `gptimage_edit_flare` — the fast model. Default to
  these. Use them for drafts, iteration, and anything we are still deciding on.
- `gptimage_generate_sunburst` / `gptimage_edit_sunburst` — the precision model.
  Roughly twice the wall time at the upper quality tiers and exactly the same
  price. Use it for the final render, and for any edit that has to change one
  thing and leave the rest of the frame untouched.
- `gptimage_result` — fetches a render that came back as a `job_id`.

Treat image generation as a normal capability you have, not as something you
need to apologize for.

- You can see every image the tools return. It arrives as a real image block.
  Look at it, judge it, and say what is wrong with it before I have to.
- When a result contains a markdown image link, put that exact link in your
  reply. That is the only thing that makes the picture appear in the
  conversation body. Do not describe the image instead of showing it.
- When a result says `Saved to <path>`, tell me the path. That file is the copy
  that outlives everything else.
- `status: "pending"` with a `job_id` is not a failure and not an outcome to
  report to me. Call `gptimage_result` with that `job_id` again, and keep
  calling until the image comes back. Never tell me to check back later: there
  is nothing for me to do, polling is your job.
- `quality` drives cost far more than anything else. `medium` is the default and
  is right for most things. `high` for text-heavy work like diagrams and slides.
  `xhigh` and `max` only when I have said the image is final: each step up
  multiplies the bill, and `max` is roughly four times `xhigh`.
- When the image needs exact text or a specific layout, send a JSON object as
  the entire prompt instead of a sentence. The model renders the structure:
  zones, roles, exact strings, hex colors, constraints. It is far more reliable
  than prose for anything with a headline, a label, or a defined arrangement.
- Editing works and takes up to 16 reference images. Every input is processed at
  full fidelity, which costs input tokens, so a multi-reference edit bills more
  than a generate. Downscale inputs first, base64 is expensive to emit.
- An error from these tools is usually a precise message from OpenAI, including
  the moderation stage and category when something is blocked. Read it, fix the
  argument or the prompt, and retry. Do not conclude the server is broken.
```

---

## Choosing between the two models

Both models take the same arguments and bill at the same rates. Measured at
1024x1024, they emit the same number of output tokens at every quality tier, so
**the cost is identical and the only trade is latency against precision.**

| Quality | Output tokens | Cost | Flare | Sunburst |
| --- | --- | --- | --- | --- |
| `low` | 196 | $0.006 | ~10 s | ~10 s |
| `medium` | 439 | $0.013 | ~11 s | ~18 s |
| `high` | 1756 | $0.053 | ~18 s | ~33 s |
| `xhigh` | 3122 | $0.094 | ~23 s | ~55 s |
| `max` | ~7024 | ~$0.21 | not measured | not measured |

Times are end-to-end through the tool, including the poll window, on a
1024x1024 render. They rise with size.

Reach for Flare by default. OpenAI calls it "the default choice for most
applications" and the numbers agree: at `medium` it is as fast as `low` and
lands inside a single tool call.

Reach for Sunburst when the job is precision editing. Its stated advantage is
changing one element while the subject, composition and surrounding detail stay
put, and holding that consistency across a run of edits. For a first draft from
a text prompt it buys you very little for double the wait.

---

## The five tools

### `gptimage_generate_flare` and `gptimage_generate_sunburst`

Text prompt in, image out. `prompt` is the only required argument. The two take
identical arguments.

| Argument | Values | Default | Notes |
| --- | --- | --- | --- |
| `prompt` | string | required | Subject, style, mood, and any text to render. Quote exact strings you want drawn. |
| `size` | `WIDTHxHEIGHT` or `auto` | `1024x1024` | Both edges divisible by 16, aspect 1:3 to 3:1, no edge over 3840, total pixels 655,360 to 8,294,400. Above 2560x1440 is experimental. |
| `quality` | `auto` `low` `medium` `high` `xhigh` | `medium` | Drives both time and cost. `max` appears only if the server's `max_quality` allows it. |
| `background` | `auto` `opaque` `transparent` | `auto` | `transparent` needs png or webp. |
| `format` | `png` `jpeg` `webp` | `webp` | webp keeps the payload small enough to survive the connector. |
| `n` | 1 to 4 | 1 | Server cap. Each one costs. |
| `compression` | 0 to 100 | 80 | webp and jpeg only, ignored for png. |

The advertised `quality` list is built from the server's own ceiling, so
whatever the schema offers you is a tier you can actually get. Ask above it and
the request is clamped down rather than refused, and the caption tells you which
tier really ran.

### `gptimage_edit_flare` and `gptimage_edit_sunburst`

Existing images in, edited image out. `images` and `prompt` are required.

| Argument | Values | Default | Notes |
| --- | --- | --- | --- |
| `images` | array of base64 strings, 1 to 16 | required | png, jpeg, or webp bytes. A `data:` URI is accepted, the prefix is stripped. |
| `prompt` | string | required | What to change, or how to combine the inputs. Name what must stay the same too. |
| `mask` | base64 png | none | Transparent pixels mark the region to change. Must match the first image's format and size, under 50 MB. |
| `size` `quality` `background` `format` `n` `compression` | as above | as above | Same as generate. `background: "transparent"` with png or webp is how you strip a background off an existing image. |

Two inputs and no mask composites them into one scene. One input and a mask is
targeted inpainting. One input and no mask rewrites the whole frame guided by
the original.

There is no `input_fidelity` argument and you do not want one: the 2.5 models
process every image input at full fidelity automatically, and the API rejects
the parameter. The cost of that shows up as image input tokens. A single
1024x1024 reference adds about 1024 of them, which at $8 per million is $0.008,
more than the whole output cost of a `low` render.

### `gptimage_result`

Takes a `job_id` and nothing else. Returns the finished image, or `pending`
again. It is annotated read-only: it starts no new work, spends no money, and is
safe to call as many times as it takes.

---

## Structured prompts for text and layout

`prompt` is a string, but the model will take a structured document as the
entire prompt and render it as layout. JSON works best. YAML, a key-value list,
or a table also work: any consistent structure is read as structure. Keys act as
instructions, values as content.

Reach for this whenever the image has to carry exact text or a defined
arrangement: posters, title cards, UI mockups, packaging, labels, diagrams,
infographics, anything with a headline and a subhead in the right places. Plain
prose prompts drift on both wording and position. A structured prompt does not.

Send the object as the whole prompt. Do not wrap it in a sentence or narrate it
first, that dilutes it. This one was rendered at `low` quality, and every zone
landed in order with all four strings legible:

```json
{
  "format": "poster",
  "canvas": {"ratio": "1:1", "background": "#0e0e11", "margin": "8%"},
  "style": {"look": "flat vector, high contrast, print-ready",
            "palette": ["#d6262a", "#f2f2f0", "#0e0e11"]},
  "layout": [
    {"zone": "top", "role": "eyebrow", "text": "SPECTER POINT",
     "case": "uppercase", "tracking": "wide", "size": "small", "color": "#f2f2f0"},
    {"zone": "upper-middle", "role": "headline", "text": "GPTIMAGE",
     "font": "condensed grotesque, heavy", "size": "very large", "color": "#d6262a"},
    {"zone": "middle", "role": "rule", "shape": "thin horizontal line",
     "color": "#d6262a", "width": "60%"},
    {"zone": "lower-middle", "role": "subhead",
     "text": "Structured prompts render as layout", "size": "medium", "color": "#f2f2f0"},
    {"zone": "bottom", "role": "caption", "text": "low quality test render",
     "size": "small", "color": "#f2f2f0", "opacity": 0.7}
  ],
  "constraints": {"text_must_be_legible": true, "no_extra_text": true,
                  "no_photographic_elements": true}
}
```

What earns its place in that object:

- A `zone` or explicit position per element, so ordering is not left to chance.
- The exact string in a `text` field. Whatever you write there is what gets
  drawn, so spell it the way it should appear, including case.
- A `style` block with a named palette. Hex values are honored.
- A `constraints` block. `no_extra_text` is the one that stops the model
  inventing filler copy around your headline.

A structured prompt costs more input tokens than a sentence (547 against 223 on
a comparable render), which is noise next to the image cost. Validate the layout
on Flare at `low`, then re-send the same object at the quality you want.

---

## How a result comes back

A render runs on a background thread. The tool call blocks up to 25 seconds
waiting for it, then returns whatever it has.

**Fast path.** Flare at `low` or `medium` finishes inside the 25 second window
and the image comes back in that first call. This is the usual case, and it is
why `medium` is the default.

**Job path.** `high` and above, Sunburst above `medium`, or a large size will
not finish in 25 seconds, so the call returns this instead:

```json
{
  "status": "pending",
  "job_id": "job_1f0c3854f156117c84a9204b",
  "instructions": "The image is still rendering ... Call the gptimage_result tool again with this exact job_id ..."
}
```

Then you call `gptimage_result` with that `job_id`. That call also blocks up to
25 seconds, so each poll is cheap in wall-clock terms and covers real ground.
Repeat until the image arrives.

Expect one or two polls for most renders and three for Sunburst at `xhigh`.
Pending jobs are never evicted, so the id stays valid for as long as the render
runs. If you get past 8 to 10 polls with nothing, say the render appears stalled
and offer to retry at lower quality. That is the only situation where reporting
back without an image is correct.

**Delivery.** A finished result carries up to three things:

1. An image content block. You can see it. Use it.
2. A text block with a markdown link, when the server is hosting renders:
   `![generated image](https://your-host/i/job_<id>-<index>.webp)`
3. A caption naming the model, the quality that actually ran, the tokens billed,
   the dollar cost, and the file path if the server is saving renders.

Reproduce that markdown link exactly as given. claude.ai renders an image from a
tool result inside the collapsed tool-call widget, where nobody looks. The link
is what puts the picture in the conversation body. Over a local stdio connection
there is no hosting and no link, only the inline image, which is fine and needs
no link.

The hosted URL needs no authentication: the random 96-bit job id is the
capability.

---

## What survives, and for how long

Three tiers, and only the third is permanent.

| Tier | Lifetime | Set by |
| --- | --- | --- |
| Inline image block in the tool result | The conversation | always |
| In-memory render behind `/i/<job_id>` | 24 h after completion | `job_ttl_seconds` |
| File on disk, served by the same URL | Until pruned or deleted | `save_dir` |

With `save_dir` unset, a hosted link dies with the memory copy and the render is
unrecoverable. With it set, every finished render is written as
`<job_id>-<index>.<ext>`, the same name the URL uses, so the route falls through
to disk once memory has let go and the link keeps working indefinitely. Locally
over stdio that same setting is what puts the file in the user's own pictures
folder without them having to ask.

`save_max_files` prunes the oldest above a count, so the directory cannot grow
until it takes the box down. Default 2000. On the deployed server the directory
is `/var/lib/gptimage` (systemd `StateDirectory=`); anywhere else under
`ProtectSystem=strict` is read-only and the save silently does not happen.

Say which of these applies when the user asks for something they intend to keep.
If `save_dir` is off, tell them the link expires and they should download it.

---

## Iterating on an image you just made

The render is fetchable at its hosted URL, so an iteration loop exists as long
as you can turn that URL into base64. With `save_dir` on and a shell available,
read the local file instead and skip the download entirely.

```bash
curl -s -o shot.webp "https://your-host/i/job_<id>-0.webp"
python -c "
from PIL import Image
import base64, io
im = Image.open('shot.webp').convert('RGB').resize((512, 512), Image.LANCZOS)
buf = io.BytesIO(); im.save(buf, 'JPEG', quality=70)
print(base64.b64encode(buf.getvalue()).decode())
" > shot.b64
```

Then pass that string in `images`. This works: a 160x160 jpeg round-tripped
through an edit tool and came back as a clean 1024x1024 recolor of the original.

Size discipline matters more than it looks, in two directions. You have to emit
the base64 as text in the tool call, so a 2 MB png is roughly 2.7 million
characters and is not happening. And every input is billed at full fidelity, so
a large reference costs real input tokens. Downscale to 512px or smaller and use
jpeg quality 60 to 70, which lands around 3 to 30 KB. The model reads
composition from the input and renders the output fresh at full size, so a small
input costs very little fidelity.

Without a shell, on claude.ai with a file the user uploaded: the same constraint
applies, and it is the real limit. Getting bytes out of an uploaded file and
into a tool argument is only practical for small images. Say that plainly
instead of claiming edits are impossible, and offer the alternative of
regenerating from a description.

---

## Claims to stop making

Each of these has been checked. They are wrong.

**"I can't see the image, so I can't tell you if it worked."**
You can. The result contains an image block and you are looking at it. Critique
it unprompted: wrong count, mangled text, wrong aspect, off style. The user
should not have to be the one who notices the hands are wrong.

**"I generated it but I can't display it here."**
Put the markdown link in your reply. That is the whole mechanism, and the tool
result tells you to do it.

**"The image is still rendering, check back in a minute."**
There is nothing for the user to check. Call `gptimage_result` again. Pending is
an intermediate state in a loop you own.

**"That job id is gone, we have to start over."**
A finished job is readable for 24 hours in memory, including after you already
showed it, and indefinitely from disk when `save_dir` is set. Re-fetching the
same `job_id` returns the same image and the same link.

**"I can only generate new images, not edit yours."**
Two edit tools exist. They inpaint with a mask, composite up to 16 inputs, and
accept png, jpeg, or webp bytes.

**"It can't do transparent backgrounds, large sizes, variants, or text."**
`background: "transparent"` with png or webp. Sizes to 4K, any dimensions
divisible by 16 within a 1:3 to 3:1 aspect. `n` up to 4. The model renders
legible text, so put the exact wording in quotes in the prompt.

**"It can't reliably place text or hit an exact layout."**
Not with a prose prompt, no. Send a JSON object as the whole prompt, with a zone
and an exact string per element. See the structured prompts section.

**"The model is fixed server-side and I can't choose one."**
You choose it by choosing the tool. Flare and Sunburst each have their own
generate and edit tool.

**"Sunburst costs more."**
It does not. Both models bill at the same rates and emit the same number of
output tokens at the same quality. You are paying in wall-clock time only.

**"The image server is broken."**
Read the error string first. It is passed through from OpenAI and is usually
exact, for example: `Invalid size '1920x1080'. Width and height must both be
divisible by 16.` A moderation block also names the stage and the category. Fix
the argument and retry.

**"I'm not able to generate images."**
The tools are in your list. Call them.

---

## Real limits

Do not promise these.

- No URL input. The edit tools take base64 bytes only.
- No seed and no determinism. The same prompt gives a different image each call.
- No streaming and no partial previews. `partial_images` exists in the API and
  this server does not use it, so there is nothing to show mid-render.
- No job list and no history. You need the `job_id` you were given.
- No older models. Only the two 2.5 models are exposed.
- Four concurrent renders. A fifth returns "server is busy" immediately, which
  is a retry-shortly condition, not a failure of the request itself.
- `n` is capped at 4 per call by the server, whatever you pass.
- Quality is capped by the server's `max_quality`. Above it you are clamped, not
  refused, so read the caption to see what actually ran.
- No video, no vector output, no upscaling except by running another edit pass.
- Prompts go through OpenAI moderation, set permissive but not off. A rejection
  comes back as an error naming the reason. Rephrasing often clears an
  over-eager block, so try once before giving up.

---

## Cost

This bills a real OpenAI account. Both models: $5 per million text input tokens,
$8 per million image input tokens, $30 per million image output tokens.

Per image at 1024x1024, output only, either model:

| `low` | `medium` | `high` | `xhigh` | `max` |
| --- | --- | --- | --- | --- |
| $0.006 | $0.013 | $0.053 | $0.094 | ~$0.21 |

Add roughly $0.008 per 1024x1024 reference image on an edit, since inputs are
always processed at full fidelity. A four-reference composite therefore costs
more in input than a `medium` render costs in output.

OpenAI publishes no static token table for these models, so every result caption
carries the real figure read back from the API's own `usage`. Trust that over
this table.

So: iterate on Flare at `low` or `medium`, confirm the composition with the
user, then render the keeper once at the quality it deserves. Do not silently
loop expensive renders trying to perfect something, and do not raise `n` to
hedge unless the user asked for options.

---

## Quick reference

| Fact | Value |
| --- | --- |
| Models | `gpt-image-2.5-flare`, `gpt-image-2.5-sunburst` |
| Poll window per call | 25 s |
| In-memory render kept for | 86400 s (24 h) |
| On-disk render kept for | until pruned at `save_max_files` (default 2000) |
| Concurrent renders | 4 |
| Max images per call | 4 |
| Max reference images per edit | 16 |
| Defaults | 1024x1024, medium, webp, compression 80, background auto |
| Quality ceiling | `xhigh` unless raised |
| Size rule | divisible by 16, aspect 1:3 to 3:1, ≤3840 per edge, ≤4K total pixels |
| Hosted render URL | `https://your-host/i/<job_id>-<index>.<ext>` |
| Saved file name | `<job_id>-<index>.<ext>` in `save_dir` |
| Storage | memory, plus `save_dir` on disk if set; never the database |
