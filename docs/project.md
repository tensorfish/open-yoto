---
icon: lucide/users
---

# Project, support & contributing

Open Yoto is independent replacement firmware for the **Yoto Mini**, not a
Yoto product or an implementation of Yoto's online services. It is an early
development project for people comfortable building firmware and working with
internal UART connections. Working features are not a claim of production
readiness or safety certification.

Start with [hardware compatibility and installation](firmware/flash-custom-firmware.md),
then [local setup](firmware/post-flash-setup.md). The
[firmware README](https://github.com/tensorfish/open-yoto/blob/main/firmware/README.md)
tracks implemented features and known limitations; the
[runtime reference](open-yoto-firmware.md) explains their behavior.

## Get help

Use [GitHub Issues](https://github.com/tensorfish/open-yoto/issues) for setup
questions, reproducible bugs, and feature proposals. Search existing issues
first. Support is community-based; there is no guaranteed response time.
For vulnerabilities, use the separate [security guidance](#security).

A useful report includes:

- **Hardware:** Yoto Mini board revision, display type, UART adapter, and power
  source. Do not identify a board by purchase year alone.
- **Build:** source commit, ESP-IDF version, selected board configuration, and
  whether any test/bring-up options were enabled.
- **Reproduction:** smallest sequence of steps, expected result, actual result,
  and whether it happens consistently. Include the last working commit if known.
- **Evidence:** relevant boot/error log excerpt and a photo or screenshot when
  useful. For audio problems, include format, codec, sample rate, and a small
  redistributable sample if available.

Do not attach full flash/NVS dumps, access codes, session cookies, device
identifiers, personal recordings, or purchased Yoto audio. Redact logs and
photos before posting. Keep original backups private and offline.

## Contributing

Useful contributions include revision-specific hardware reports, setup-guide
corrections, accessibility improvements, reproducible bug reports, and focused
firmware fixes. You do not need to reverse engineer a peripheral to help.

1. Discuss substantial changes in an issue before implementing them. State the
   user problem, hardware affected, and proposed behavior.
2. Follow the [build guide](build-setup.md) and existing component boundaries.
   `firmware/components/board/board_pins.h` is the authoritative pin map;
   `firmware/main/app_main.c` owns player state.
3. Keep a pull request focused. Explain what changed and why, link the issue,
   and separate verified observations from assumptions.
4. Exercise the affected behavior. State board revision, configuration, and
   exact checks performed. Explicitly label changes not tested on hardware.
5. Update the relevant docs and implemented/limited status when behavior changes.
   Preserve third-party notices; contribute only material you have rights to share.

From the repository root, the existing host checks run with a C compiler and Bash:

```bash
bash firmware/test/host/run.sh
```

These cover selected control, display, and power policies; they do not validate
physical charging, audio quality, radio operation, or electrical safety.
Hardware-affecting changes need evidence from the affected revision, not just
a passing host run or build.

For documentation changes:

```bash
uv sync --locked
uv run zensical build --strict
```

The landing page is plain HTML/CSS in `landing-page/`, with no build step.
Preview it through a local static server and check narrow and wide viewports,
keyboard navigation, and links. Never hand-edit generated `site/` output.

## Security

### Local does not mean encrypted

Wi-Fi administration starts only after scanning the exact admin-card URL.
The `openyoto` access point is **open**, and the page at `http://192.168.4.1`
uses **HTTP, not HTTPS**. The displayed six-character code authenticates access;
it does not encrypt credentials, session cookies, uploads, or other traffic.
Nearby radio observers may be able to capture traffic. Do not administer the
player in an untrusted public environment or bridge its hotspot to the internet.

Use a trusted phone/computer and web UI from the same source revision as the
firmware. The SD-hosted interface is executable browser content: do not install
unknown replacement pages. Stop admin mode by scanning the admin card again
when finished. Bluetooth is separately opt-in; disable it when not needed.
Physical access to the player or SD card is outside the protection offered by
the web login. Do not treat the player as secure storage.

### Report a vulnerability

The project's chosen channel is **GitHub private vulnerability reporting**.
Check the repository's [Security page](https://github.com/tensorfish/open-yoto/security)
for **Report a vulnerability**. Include the affected commit, board, reproduction,
impact, and any suggested mitigation. Do not publish exploit details in an issue.

!!! warning "Repository setting still required"
    At the 8 September 2026 documentation audit, GitHub reported private
    vulnerability reporting as disabled. A repository administrator must enable
    it under **Settings → Advanced Security → Private vulnerability reporting**.
    Until the private reporting option is available, open an issue asking only
    for a private contact route; omit vulnerability details and sensitive data.

There is no published supported-version window or security-response deadline.
Do not assume a development snapshot receives backported fixes.

## Licensing

Original Open Yoto code and documentation are licensed under the
[MIT License](https://github.com/tensorfish/open-yoto/blob/main/LICENSE).
That licence permits reuse and modification subject to preserving its notice;
it does **not** grant rights to material owned by others.

The repository is not a uniformly MIT-licensed firmware distribution. Known
items needing separate treatment include:

| Material | Current evidence and limitation |
|---|---|
| `firmware/components/libhelix-aac/` and `libhelix-mp3/` | Source headers carry RealNetworks RPSL/RCSL notices. Preserve them; the project MIT licence does not override their terms. |
| ESP-IDF and managed components, including `esp_audio_codec` | Upstream licences and binary redistribution terms apply independently; inspect the versions resolved for a build. |
| `firmware/components/aw88194/factory_aw88194_fw.bin` and `factory_aw88194_cfg_rev04.bin` | Recovered amplifier firmware/configuration. Redistribution rights are not established by the project licence. |
| `firmware/components/yoto_vfs/stock_welcome.m4a` | Stock-derived audio embedded in the firmware; not covered by the original-code MIT grant. |
| Extracted `assets/`, stock-derived artwork, and generated derivatives | Extraction or conversion does not establish permission to redistribute. Provenance and rights require review. |

A dependency and asset rights audit remains necessary before publishing a
redistributable firmware bundle. Record provenance and applicable terms, obtain
permission where needed, or replace material with appropriately licensed
alternatives. This list is an initial inventory, not legal clearance or a
complete software bill of materials. Do not upload stock dumps or paid content
as contributions. Yoto names and product imagery do not imply endorsement.

## Release readiness

As of the 8 September 2026 audit, the repository has no published GitHub
releases and no checked-in CI workflow. Installation is a source-build workflow,
not a stable binary download. The following are **remaining work**, not promises
of features or a release schedule:

| Priority | Gap | Evidence needed before calling it ready |
|---|---|---|
| Before distributing binaries | Dependency and stock-asset rights | Complete notices/provenance inventory and resolved redistribution permissions or replacements. |
| Before inviting sensitive reports | Private security channel | Enable GitHub private reporting and verify the reporter-facing flow. |
| Before a supported release | Revision-specific validation | Record a commit/configuration and physical results for #04 and #05: install, playback, NFC write/read-back, admin, headphones, Bluetooth, charging, shutdown/wake, and recovery. Mark missing checks and display limitations. |
| Before repeatable releases | Build and regression automation | Pin toolchain/dependency inputs; automate both board builds and host checks, plus docs/link checks. Keep generated firmware tied to its source. |
| With each binary release | Release identity and integrity | Publish board-specific artefacts, source commit, build configuration, matching web UI, changes, known issues, and SHA-256 checksums. Document authenticity verification separately; checksums alone do not authenticate a publisher. |
| Before promising safe upgrades | Upgrade and recovery evidence | Demonstrate SD/config preservation, rollback with that player's backup, failure recovery, and document the supported update path. The current firmware has no OTA update slots. |
| Before broader family use | Safety and usability evidence | Assess battery/charging behavior, speaker/headphone volume limits, long-running playback, and setup on both revisions. Do not equate software checks with product safety certification. |

### What informed these docs

These projects provide useful documentation patterns, not endorsements of Open Yoto:

- [OpenWrt quick start](https://openwrt.org/docs/guide-quick-start/start): identify
  the exact hardware and read device-specific installation instructions first.
- [Rockbox](https://www.rockbox.org/): distinguish stable, incomplete, and
  unsupported ports rather than presenting every target as equally ready.
- [Tasmota getting started](https://tasmota.github.io/docs/Getting-Started/): make
  electrical prerequisites, serial setup, and initial configuration explicit.
- [ESPHome security guidance](https://esphome.io/guides/security_best_practices/):
  state trust assumptions and limitations alongside security features.
