<div align="center">

<img src="assets/gptimage.ico" width="96" alt="GPTImage">

# GPTImage

**Bolts OpenAI's ChatGPT Images 2.5 into your chat window so you stop alt-tabbing like some kind of animal.**

[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
![platform](https://img.shields.io/badge/platform-C%2B%2B20-d6262a?style=flat-square)

</div>

---

An MCP server that bolts OpenAI's GPT Image 2.5 models onto Claude so you can
stop alt-tabbing between two chat windows like some kind of animal.

You ask Claude for a picture. Claude calls a tool. The picture shows up in the
chat. That is the entire trick. No browser automation, no scraping ChatGPT's
web UI, no cursed Selenium rig held together with duct tape and a prayer. Just
the real API, wrapped in the Model Context Protocol, handing the image back
inline where you can actually see it.

The models are `gpt-image-2.5-flare` and `gpt-image-2.5-sunburst`, marketed at
you as "ChatGPT Images 2.5." Flare is the fast one, Sunburst is the fussy one.
They do legible text, 4K output, transparent backgrounds, and do not smear faces
into Lovecraftian horror nearly as often as their ancestors did.

## What it actually does

Each model gets its own generate and edit tool, so the model is picked by which
tool Claude calls rather than buried in an argument it will forget to set. A
fifth tool fetches a slow render. Scope creep is how projects die in a ditch.

- **`gptimage_generate_flare`** — text in, image out, on the fast model. The
  default for anything you are still thinking about.
- **`gptimage_generate_sunburst`** — the same, on the precision model. Slower,
  for the render that is actually going in the deck.
- **`gptimage_edit_flare`** — hand it up to sixteen images plus a prompt and it
  edits or mashes them together. Pass a `mask` for surgical inpainting instead
  of "regenerate the whole damn thing and hope."
- **`gptimage_edit_sunburst`** — the same, on the model built for it. Editing is
  what Sunburst is for: change the one thing you asked about and leave the rest
  of the frame alone, across several passes without the picture degrading.
- **`gptimage_result`** — an `xhigh` render takes the better part of a minute,
  longer than a connector will sit and wait, so the other four hand back a
  `job_id` and this fetches the finished image once it is ready.

Images come back as webp, a few dozen KB instead of a multi-megabyte PNG that a
remote connector quietly drops on the floor. Over that connector the server also
hosts each render and hands Claude a link to it, so the picture lands inline in
the conversation body instead of collapsed inside a tool-call widget you have to
expand. Locally over stdio there is nothing to host, so the image rides back
inline as base64 and the client renders it.

Set `save_dir` and every finished render is also written to disk, named the same
as its hosted URL. Two things follow: you have the file without having to
remember to save it, and the link keeps working after the in-memory copy expires
instead of 404ing a day later, which is the failure everybody hits exactly once.

## The part you will ignore until it bites you

This thing spends real money. Measured on either model at 1024x1024, one image
costs about half a cent at `low`, 1.3 cents at `medium`, 5.3 cents at `high` and
9.4 cents at `xhigh`; `max` is around 21 cents. It is single-tenant by design:
your key, your box, your problem. `max_n` caps images per call and
`max_quality` caps the tier, which is the one that actually runs up a bill, but
the cap on how often you press the button is your own self-control. Godspeed.

## The stuff you need

- A C++20 compiler (MSVC 2022, or gcc-13 / clang-17) and CMake 3.25+.
- PostgreSQL 16+. Yes, an image server wants a database, and no, it never writes
  an image to it. The Postgres schema holds only the auth plumbing: static bearer
  tokens and the OAuth server's clients, codes, and refresh tokens. A finished
  render lives in memory for a day, set by `job_ttl_seconds`, served from an
  unguessable per-render URL and then dropped. It touches the disk only if you
  set `save_dir`, and only as an ordinary image file in the directory you named.
- An OpenAI API key with image access, in the `OPENAI_API_KEY` environment
  variable. It never goes in a config file. If you paste your key into a TOML
  and commit it, that is a you problem.

## Build it

Windows (MSVC, multi-config):

```
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target gptimage_mcp gptimage_cli
```

Binaries land at `build\src\mcp\Release\gptimage_mcp.exe` and
`build\src\cli\Release\gptimage_cli.exe`. The libpq runtime DLLs get copied
next to them automatically, so they run without PostgreSQL's `bin` on PATH.

Linux:

```
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DGPTIMAGE_BUILD_TESTS=OFF
cmake --build build -j"$(nproc)" --target gptimage_mcp gptimage_cli
```

First configure downloads the dependencies (nlohmann/json, spdlog, cpr,
toml++, cpp-httplib, doctest) via CMake FetchContent. Go get a coffee.

## Wire it into Claude, the lazy local way

For local use over stdio, GPTImage never even opens the database: auth is off
(stdio trusts the OS boundary) and the tools only talk to OpenAI. You need a
config with a `[database]` block present (the parser insists on `dbname` and
`user`, even though nothing connects) and your API key in the env. Copy
`config/gptimage.toml.example` to `config/gptimage.toml`, then drop this into
your `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "gptimage": {
      "command": "C:\\path\\to\\GPTImage\\build\\src\\mcp\\Release\\gptimage_mcp.exe",
      "args": ["--transport", "stdio"],
      "env": { "OPENAI_API_KEY": "sk-your-key-here" }
    }
  }
}
```

Restart Claude Desktop. Ask it to generate an image. Watch the magic. Or watch
the error message, in which case read it, because it will tell you exactly what
you did wrong.

## Wire it into Claude, the grown-up remote way

Run it as an HTTP server on a box with a domain, front it with a TLS proxy, and
add it to claude.ai as a custom connector. GPTImage ships an embedded OAuth 2.1
authorization server, so the "add custom connector" flow just works: point it at
`https://your-host/mcp`, sign in on the built-in login page, done. Set a login
password with `gptimage_cli passwd operator` first.

Prefer a static token? Mint one with `gptimage_cli tokens add` and bridge stdio
to the remote server with `mcp-remote`:

```json
{
  "mcpServers": {
    "gptimage": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "https://your-host/mcp",
               "--header", "Authorization:${GPTIMG_AUTH}"],
      "env": { "GPTIMG_AUTH": "Bearer gpt_your_token_here" }
    }
  }
}
```

## Deploy to a VPS without crying

Everything you need is in `deploy/` and `scripts/`:

- `deploy/gptimage-mcp.service` — hardened systemd unit, binds `127.0.0.1:17718`.
- `deploy/Caddyfile.gptimage.snippet` — Caddy site block, terminates TLS, proxies
  the MCP and OAuth paths plus `/i/*` (the hosted-render route), 404s the rest.
- `deploy/fail2ban/` — jails that ban whoever brute-forces your login or spams
  the DCR endpoint.
- `scripts/deploy_vps.sh` — pulls from git, builds, runs migrations, atomically
  flips a release symlink, health-checks, and rolls itself back if the new
  build faceplants.
- `scripts/install_linux.sh` — the one-time host-setup checklist.

One-time database bootstrap:

```
createdb -U postgres gptimage
psql -U postgres -d gptimage -v gptimage_pw='...' -f sql/setup/create_role.sql
gptimage_cli migrate
gptimage_cli oauth keygen
```

Secrets live in `/etc/gptimage/env` as an EnvironmentFile: `GPTIMAGE_DB_PASSWORD`
and `OPENAI_API_KEY`. Not in the TOML. We have been over this.

## Configure the knobs

All of it lives in `config/gptimage.toml` (see the `.example`). The `[image]`
section is the interesting bit: default size, default quality, output format
(`webp` by default, so a render stays light enough to display inline) and its
compression level, the model id (bump it when OpenAI ships the next one and
change nothing else), and `max_n` to keep runaway agents from setting your
credit card on fire. `public_base_url` is the origin renders are served from;
leave it blank and it inherits your OAuth issuer, so the remote deploy needs no
extra wiring.

## Test

```
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

The unit tests cover config parsing, the auth/grant model, JWT signing, the
OAuth PKCE helpers, password hashing, and the async render cache (including the
hosted-URL result shaping). They do not call OpenAI, so they cost nothing and
prove nothing about whether the pictures are any good.

## License

GPLv3. It is in `LICENSE`, all 553 glorious lines of it. Use it, fork it, ship
it, but if you distribute it you share the source. Those are the rules. Take
them up with the Free Software Foundation, not me.

Built by Locke Werks.
