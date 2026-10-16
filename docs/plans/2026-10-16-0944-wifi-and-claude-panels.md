# Wi-Fi and Claude panels

Date: 2026-10-16 09:44 EDT

## Context

The bar's network panel takes the look and contents of the network panel of the shell the user came from
(Omarchy's `omarchy.network`), and the bar gains a Claude panel modelled on that shell's `omarchy.agents` panel.
Both shells' panels are on the user's machine to compare against, with their sources readable under
`/usr/share/omarchy/shell/`; nothing of that shell is run or read at run time — the bar keeps reading only the
system's own services and the tools' own records ([shell/bar.md](../intents/shell/bar.md)).

The network panel opens with the connection in use — its name, its state, the Wi-Fi switch, and buttons to share
the network as a QR code and to run a speed test where the system has the tools. Below it come live figures (ping,
packet loss, receive and send rates, downloaded and uploaded totals, the IP address and the gateway, both
copyable), the band in use with an Automatic switch (off, it locks the connection to 2.4, 5 or 6 GHz), the DNS
provider as four choices (DHCP, Cloudflare, Google, Custom), the known networks with the connected one first, and
the other networks the scan finds while the panel is open. Joining a network asks for a password, or an identity
and passphrase on an enterprise network; a known network can be forgotten.

The Claude panel hangs from a bar icon that appears only on a machine with Claude usage. Its head shows the mark,
"Claude Code" and the plan, or a state such as waiting for sign-in, repeated in a card that says how to sign in.
With a signed-in CLI it shows the limits: the 5-hour session and the 7-day week as a percentage with a meter and
the time to the reset. It always shows tokens by day for the last week, today in bold at the bottom, and tokens by
model with a bar behind each row scaled to the heaviest model. The figures come from Claude Code's transcripts
and from pi/omp and opencode sessions that ran on an Anthropic provider; the limits come from Anthropic's OAuth
usage endpoint with the CLI's own credentials. On the same machine the figures equal the ones the other shell
shows.

## Shared

- [ ] UI kit: a panel hero, a section header and a separator, in both modes

## Network panel

- [ ] Head: the connection's name and state, the Wi-Fi switch; share-as-QR and speed test where the system has the tools
- [ ] Live figures: ping, packet loss, receive and send rates, downloaded and uploaded, IP address and gateway (copyable)
- [ ] Band: the band in use and an Automatic switch; off, a choice of 2.4, 5 and 6 GHz for the connection
- [ ] DNS provider: DHCP, Cloudflare, Google, Custom (servers typed in), applied to the active connection
- [ ] Known networks first (the connected one on top, lock, forget), then the networks the scan finds while the panel is open
- [ ] Joining: a password for secured networks without credentials, identity and passphrase for enterprise ones

## Claude panel

- [ ] Usage collector: tokens by day for the last 7 days and by model, from Claude Code transcripts and pi/omp and opencode sessions on an Anthropic provider
- [ ] Limits from the OAuth usage endpoint with the CLI's credentials: 5-hour session and 7-day week, percentage, meter, reset time; the plan's name
- [ ] Bar icon in the right section before Bluetooth; hidden on a machine without Claude usage
- [ ] Panel: hero with the mark, the tool and the plan or state; a sign-in card without credentials; tokens by day with today in bold; tokens by model with bars

## Verification

- [ ] The bar scenario drives both panels in its private world: band, DNS, joining and forgetting reach the NetworkManager stand-in; the Claude panel reads fixture sessions and a stand-in usage endpoint; nothing reaches the machine's network settings, its credentials or Anthropic
- [ ] Screenshots of both panels in both modes, inspected against the reference screenshots
- [ ] Read-only on this machine: the Claude figures equal the other shell's record, the network panel shows the live connection
- [ ] The bar intent states both panels; warning-free build, every suite green
- [ ] On screen: both panels, nested and on a TTY (the user)
