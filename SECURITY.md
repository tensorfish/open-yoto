# Security

Open Yoto is development firmware. No supported-version window or response
service level is currently published.

## Private reports

Use **Report a vulnerability** on the repository's
[Security page](https://github.com/tensorfish/open-yoto/security) when available.
Include the affected commit, hardware revision, reproduction steps, and impact.
Do not post exploit details, flash dumps, credentials, or personal data publicly.

GitHub private vulnerability reporting is the selected channel, but was
**disabled at the 8 September 2026 audit**. A repository administrator must
enable it in Settings → Advanced Security → Private vulnerability reporting.
Until then, open an issue requesting a private contact route only, without
vulnerability details.

## Security boundaries

The admin hotspot is open Wi-Fi and uses unencrypted HTTP. The six-character
login code is authentication, not transport encryption. Use admin mode only in
a trusted environment and turn it off when finished. See the
[security guidance](docs/project.md#security) for the full boundary and safe-use
instructions.
