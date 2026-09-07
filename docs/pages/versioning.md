# Versioning

This module uses [Semantic Versioning](https://semver.org/) (`MAJOR.MINOR.PATCH`) with the following conventions.

## Scheme

```
0 . <testnet> . <patch>
│        │           └── module release within a testnet
│        └────────────── tracks the Logos testnet number
└─────────────────────── fixed at 0; breaking changes are expected
```

### MAJOR — fixed at `0`

The major version is kept at `0` for the foreseeable future. A value of `0` signals that the API is **not yet stable**: breaking changes may be introduced in any release. Once the API stabilises across testnets this policy will be revisited.

### MINOR — Logos testnet number

The minor version is incremented to match the Logos testnet with which this module is compatible. Bumping MINOR resets PATCH to `0`.

Example: `0.2.0` targets Logos Testnet 2.

### PATCH — module release within a testnet

The patch version is incremented for every module release made against the same testnet: bug fixes, dependency updates, performance improvements, or any other change that does not change the target testnet.

Example: `0.2.3` is the fourth release of the module targeting Logos Testnet 2.

## Examples

| Version | Meaning |
|---------|---------|
| `0.1.0` | First release for Testnet 1 |
| `0.1.1` | Bug-fix release, still targets Testnet 1 |
| `0.2.0` | First release for Testnet 2 |
| `0.2.1` | Patch release for Testnet 2 |

## Releasing

1. Update `"version"` in [`metadata.json`](../../metadata.json).
2. Tag the commit: `git tag v<version>`.
3. Push the tag: `git push origin v<version>`.
