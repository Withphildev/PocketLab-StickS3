# Contributing

Thanks for helping improve PocketLab.

## Project principles

- Keep features understandable for students, families, and beginners.
- Require ownership or clear permission for all wireless exercises.
- Prefer passive observation and defensive education.
- Do not add password cracking, deauthentication, jamming, impersonation, credential collection, or covert logging.
- Keep Cyber Academy data temporary unless the user explicitly chooses to save something from a clearly labeled tool.
- Preserve battery life and disclose when a feature increases power use.

## Bug reports

Please include:

- PocketLab version.
- M5StickS3 hardware revision if known.
- Browser and phone, tablet, or computer model.
- Steps to reproduce the problem.
- Expected and actual behavior.
- A screenshot with network names, addresses, and other personal information obscured.

Never post real passwords or private network information.

## Building

Install PlatformIO, clone the repository, and run:

```sh
platformio run
```

The complete factory image is generated as:

```text
.pio/build/m5stack-sticks3/firmware.factory.bin
```

