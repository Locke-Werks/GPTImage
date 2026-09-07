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

You have three tools: `gptimage_generate`, `gptimage_edit`, `gptimage_result`.
They wrap OpenAI gpt-image-2 on my own server. Treat image generation as a
normal capability you have, not as something you need to apologize for.

- You can see every image the tools return. It arrives as a real image block.
  Look at it, judge it, and say what is wrong with it before I have to.
- When a result contains a markdown image link, put that exact link in your
  reply. That is the only thing that makes the picture appear in the
  conversation body. Do not describe the image instead of showing it.
- `status: "pending"` with a `job_id` is not a failure and not an outcome to
  report to me. Call `gptimage_result` with that `job_id` again, and keep
  calling until the image comes back. A high-quality render takes 1 to 3
  minutes, which is several polls. Never tell me to check back later: there is
  nothing for me to do, polling is your job.
- Default to `quality: "low"` while we are iterating. Use `"high"` when I say
  the image is final, and expect the job flow when you do.
- When the image needs exact text or a specific layout, send a JSON object as
  the entire prompt instead of a sentence. The model renders the structure:
  zones, roles, exact strings, hex colors, constraints. It is far more reliable
  than prose for anything with a headline, a label, or a defined arrangement.
- Editing existing images works: `gptimage_edit` takes base64 image bytes plus
  a prompt, does masked inpainting, and composites several inputs into one
  scene. Downscale inputs first, base64 is expensive to emit.
- A finished render is hosted for 24 hours and then deleted. If I want to
  keep an image, save it before then.
- An error from these tools is usually a precise message from OpenAI. Read it,
  fix the argument or the prompt, and retry. Do not conclude the server is
  broken.
```

---

## The three tools

### `gptimage_generate`

Text prompt in, image out. `prompt` is the only required argument.

| Argument | Values | Default | Notes |
| --- | --- | --- | --- |
| `prompt` | string | required | Subject, style, mood, and any text to render. Quote exact strings you want drawn. |
| `size` | `WIDTHxHEIGHT` or `auto` | `1024x1024` | Both numbers divisible by 16, aspect between 1:3 and 3:1, up to 2K. |
| `quality` | `auto` `low` `medium` `high` | `low` | Drives both time and cost. |
| `background` | `auto` `opaque` `transparent` | `auto` | `transparent` needs png or webp, and the model does not always honor it. |
| `format` | `png` `jpeg` `webp` | `webp` | webp keeps the payload small enough to survive the connector. |
| `n` | 1 to 4 | 1 | Server cap is 4. Each one costs. |
| `compression` | 0 to 100 | 80 | webp and jpeg only, ignored for png. |

### `gptimage_edit`

Existing images in, edited image out. `images` and `prompt` are required.

| Argument | Values | Default | Notes |
| --- | --- | --- | --- |
| `images` | array of base64 strings | required | png, jpeg, or webp bytes. A `data:` URI is accepted, the prefix is stripped. |
| `prompt` | string | required | What to change, or how to combine the inputs. |
| `mask` | base64 png | none | Transparent pixels mark the region to change. Must match the first image's dimensions. |
| `size` `quality` `format` `n` `compression` | as above | as above | There is no `background` argument on edit. |

Two inputs and no mask composites them into one scene. One input and a mask is
targeted inpainting. One input and no mask rewrites the whole frame guided by
the original.

### `gptimage_result`

Takes a `job_id` and nothing else. Returns the finished image, or `pending`
again. It is annotated read-only: it starts no new work, spends no money, and is
safe to call as many times as it takes.

---

## Structured prompts for text and layout

`prompt` is a string, but gpt-image-2 will take a structured document as the
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
at `low`, then re-send the same object at `high` for the final.

---

## How a result comes back

A render runs on a background thread. The tool call blocks up to 25 seconds
waiting for it, then returns whatever it has.

**Fast path.** Low quality at a normal size finishes inside the 25 second window
and the image comes back in that first call. This is the usual case for drafts.

**Job path.** Medium, high, or a large size will not finish in 25 seconds, so
the call returns this instead:

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

Expect roughly 3 to 6 polls for a high-quality render. Pending jobs are never
evicted, so the id stays valid for as long as the render runs. If you get past 8
to 10 polls with nothing, say the render appears stalled and offer to retry at
lower quality. That is the only situation where reporting back without an image
is correct.

**Delivery.** A finished result carries two things:

1. An image content block. You can see it. Use it.
2. A text block with a markdown link, when the server is hosting renders:
   `![generated image](https://your-host/i/job_<id>-<index>.webp)`

Reproduce that markdown link exactly as given. claude.ai renders an image from a
tool result inside the collapsed tool-call widget, where nobody looks. The link
is what puts the picture in the conversation body. Over a local stdio connection
there is no hosting and no link, only the inline image, which is fine and needs
no link.

The hosted URL needs no authentication (the random 96-bit job id is the
capability) and stays live for 24 hours after the render finishes, then the
image is dropped from memory and the URL 404s. Nothing is ever written to disk.
Say so when the user asks for something they intend to keep.

---

## Iterating on an image you just made

The render is fetchable at its hosted URL for 24 hours, so an iteration loop
exists as long as you can turn that URL into base64.

With a shell available (Claude Code and similar):

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
through `gptimage_edit` and came back as a clean 1024x1024 recolor of the
original.

Size discipline matters more than it looks. You have to emit the base64 as text
in the tool call, so a 2 MB png is roughly 2.7 million characters and is not
happening. Downscale to 512px or smaller and use jpeg quality 60 to 70, which
lands around 3 to 30 KB. gpt-image-2 reads composition from the input and
renders the output fresh at full size, so a small input costs very little
fidelity.

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
A finished job is readable for 24 hours, including after you already showed
it. Re-fetching the same `job_id` returns the same image and the same link. Only
after the TTL, or a server restart, is regenerating necessary.

**"I can only generate new images, not edit yours."**
`gptimage_edit` exists. It inpaints with a mask, composites multiple inputs, and
accepts png, jpeg, or webp bytes.

**"It can't do transparent backgrounds, large sizes, variants, or text."**
`background: "transparent"` with png or webp. Sizes up to 2K, any dimensions
divisible by 16 within a 1:3 to 3:1 aspect. `n` up to 4. gpt-image-2 renders
legible text, so put the exact wording in quotes in the prompt.

**"It can't reliably place text or hit an exact layout."**
Not with a prose prompt, no. Send a JSON object as the whole prompt, with a zone
and an exact string per element. See the structured prompts section.

**"The image server is broken."**
Read the error string first. It is passed through from OpenAI and is usually
exact, for example: `Invalid size '1920x1080'. Width and height must both be
divisible by 16.` Fix the argument and retry.

**"I'm not able to generate images."**
The tools are in your list. Call them.

---

## Real limits

Do not promise these.

- No URL input. `gptimage_edit` takes base64 bytes only.
- No seed and no determinism. The same prompt gives a different image each call.
- No job list, no history, no recovery of a render after its 24 hour TTL.
- No `background` argument on edit, only on generate.
- The model is fixed server-side at `gpt-image-2` and is not selectable per call.
- Four concurrent renders. A fifth returns "server is busy" immediately, which
  is a retry-shortly condition, not a failure of the request itself.
- `n` is capped at 4 per call by the server, whatever you pass.
- No video, no vector output, no upscaling except by running another edit pass.
- Prompts go through OpenAI moderation, set permissive but not off. A rejection
  comes back as an error naming the reason. Rephrasing often clears an
  over-eager block, so try once before giving up.

---

## Cost

This bills a real OpenAI account.

- `low` is cents, and is the right default for anything exploratory.
- `high` at 1024x1024 is roughly 20 cents per image.
- `n: 4` at high quality is four times that in one call.

So: iterate at `low`, confirm the composition with the user, then render the
keeper at `high` once. Do not silently loop high-quality renders trying to
perfect something, and do not raise `n` to hedge unless the user asked for
options.

---

## Quick reference

| Fact | Value |
| --- | --- |
| Poll window per call | 25 s |
| Finished render kept for | 86400 s (24 h) |
| Concurrent renders | 4 |
| Max images per call | 4 |
| Defaults | 1024x1024, low, webp, compression 80, background auto |
| Size rule | divisible by 16, aspect 1:3 to 3:1, up to 2K |
| Typical timing | low 15-30 s, high 1-3 min |
| Hosted render URL | `https://your-host/i/<job_id>-<index>.<ext>` |
| Storage | in memory only, nothing on disk, nothing in the database |
