# Agent Instructions

All coding agents working in this repository **must** read and follow the rules in:

- [`.github/copilot-instructions.md`](.github/copilot-instructions.md)
- [`docs/specs/xep-0384-reference.md`](docs/specs/xep-0384-reference.md) for OMEMO protocol constraints

That file is the authoritative source for:

- Build and test commands
- Code style (C++23, clang-format, naming conventions)
- Memory management rules (RAII mandatory, `malloc`/`free`/`new`/`delete` forbidden)
- XMPP/Strophe patterns
- LMDB usage
- Architecture overview
- Git commit conventions

Before making any changes, read `.github/copilot-instructions.md` in full.

For OMEMO troubleshooting tasks, agents may use `tools/correlate_omemo_xml.sh --account <account>` as a convenience helper to correlate WeeChat log events with raw XML.
