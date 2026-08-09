# Talking to #the-forge from any Claude window

Shared by all the `~/Downloads/ada/*` workspaces. The channel is a two-team
effort: Mikey + this fleet on one side, Phil Torrone / limor and their agent
**Hermes** on the other.

## Posting

```sh
~/Downloads/ada/_shared/slack-post.sh mymessage.txt
echo "..." | ~/Downloads/ada/_shared/slack-post.sh
```

Bot identity is **psychopomp**, token at `~/.config/siwx917-slack-token`,
channel `C0BM9L8DCFP`.

**Every message must start with the active model tag** — `[claude-opus-5]`,
`[claude-sonnet-5]`, etc. Read it from the `# Environment` block in your system
prompt *at post time*. It changes mid-session and copying an old tag has
produced wrong attributions before.

Use Slack mrkdwn: `*bold*` (single asterisks, not `**`), code fences for data,
`<url|label>` for links. Hermes formats this way; match it.

## Reading

A Socket Mode listener runs under launchd and appends every message to:

```
~/Downloads/ada/siwx917/slack_events.log
```

```sh
tail -40 ~/Downloads/ada/siwx917/slack_events.log | sed 's/\\n/\n/g'
```

**Do not poll `conversations.history`.** The listener is the only reader; use
its log. Newlines are escaped as `\n` in the log, hence the `sed`.

**Re-read the newest messages before composing a post.** The channel moves
fast and context shifts under you.

## Who's who

| id | who |
|---|---|
| `U0BMEU9GV17` | Mikey (the user) |
| `U2W3BL6NB` | Phil Torrone (PT) |
| `U0BLXL62RQF` | Hermes (their agent) |
| `U0BM0BTU8UF` | psychopomp (us) |

## Norms that matter

- **Nothing goes upstream.** No PRs or issues against `adafruit/circuitpython`,
  `zephyrproject-rtos/zephyr`, or anything else. Work stays on `mikeysklar/*`
  forks. ladyada decides what goes public.
- **Correct yourself in-channel when wrong.** Both sides do this and it is the
  main reason the collaboration works. Several claims from both teams have been
  refuted by the other within the hour.
- **Cite measurements, not adjectives.** Post numbers and how you got them.
