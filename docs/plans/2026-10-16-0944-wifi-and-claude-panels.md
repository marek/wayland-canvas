# Wi-Fi and AI panels

Date: 2026-10-16 09:44 EDT

## Context

The bar's network panel takes the look and contents of the network panel of the shell the user came from
(Omarchy's `omarchy.network`), and the bar gains an AI usage panel modelled on that shell's `omarchy.agents`
panel. Both shells' panels are on the user's machine to compare against, with their sources readable under
`/usr/share/omarchy/shell/`; nothing of that shell is run or read at run time — the bar keeps reading only the
system's own services and the tools' own records ([shell/bar.md](../intents/shell/bar.md)).

The network panel opens with the connection in use — its name, its state, the Wi-Fi switch, and buttons to share
the network as a QR code and to run a speed test where the system has the tools. Below it come live figures (ping,
packet loss, receive and send rates, downloaded and uploaded totals, the IP address and the gateway, both
copyable), the band in use with an Automatic switch (off, it locks the connection to 2.4, 5 or 6 GHz), the DNS
provider as four choices (DHCP, Cloudflare, Google, Custom), the known networks with the connected one first, and
the other networks the scan finds while the panel is open. Joining a network asks for a password, or an identity
and passphrase on an enterprise network; a known network can be forgotten.

The AI panel hangs from a bar icon that appears only on a machine with AI coding usage, and covers every AI
family the machine has used, Claude and GPT first: a switch between them when there is more than one. A tab's head
shows the family's mark, its name and the plan, or a state such as waiting for sign-in, repeated in a card that
says how to sign in. Where the family has a subscription with limits and its CLI is signed in, the tab shows them:
Claude's 5-hour session and 7-day week from Anthropic's OAuth usage endpoint, GPT's from the Codex CLI, each as a
percentage with a meter and the time to the reset. Every tab shows tokens by day for the last week, today in bold
at the bottom, and tokens by model with a bar behind each row scaled to the heaviest model. The figures come from
the coding tools' own records: Claude Code's transcripts, Codex's sessions, and pi/omp and opencode sessions, each
counted under the family its provider and model belong to. On the same machine the Claude figures equal the ones
the other shell shows.

## Shared

- [ ] UI kit: a panel hero, a section header and a separator, in both modes

## Network panel

- [ ] Head: the connection's name and state, the Wi-Fi switch; share-as-QR and speed test where the system has the tools
- [ ] Live figures: ping, packet loss, receive and send rates, downloaded and uploaded, IP address and gateway (copyable)
- [ ] Band: the band in use and an Automatic switch; off, a choice of 2.4, 5 and 6 GHz for the connection
- [ ] DNS provider: DHCP, Cloudflare, Google, Custom (servers typed in), applied to the active connection
- [ ] Known networks first (the connected one on top, lock, forget), then the networks the scan finds while the panel is open
- [ ] Joining: a password for secured networks without credentials, identity and passphrase for enterprise ones

## AI panel

- [ ] Usage collector: tokens by day for the last 7 days and by model, per family, from Claude Code transcripts, Codex sessions and pi/omp and opencode sessions, one rule naming a record's family
- [ ] Claude limits from the OAuth usage endpoint with the CLI's credentials: 5-hour session and 7-day week, percentage, meter, reset time; the plan's name
- [ ] GPT limits and plan from the Codex CLI's app-server, the same way
- [ ] Bar icon in the right section before Bluetooth; hidden on a machine without AI usage
- [ ] Panel: a switch between the families with usage; hero with the mark, the family and the plan or state; a sign-in card without credentials; tokens by day with today in bold; tokens by model with bars

## Verification

- [ ] The bar scenario drives both panels in its private world: band, DNS, joining and forgetting reach the NetworkManager stand-in; the AI panel reads fixture sessions of two families, one signed in and one not, through stand-ins for Anthropic's endpoint and the Codex app-server; nothing reaches the machine's network settings, its credentials, Anthropic or OpenAI
- [ ] Screenshots of both panels in both modes, inspected against the reference screenshots
- [ ] Read-only on this machine: the Claude figures equal the other shell's record, every family with usage has a tab, the network panel shows the live connection
- [ ] The bar intent states both panels; warning-free build, every suite green
- [ ] On screen: both panels, nested and on a TTY (the user)
