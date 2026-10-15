# Changelog

## 0.1.1 - 2026-10-15

- Remove FM3 Plus explanatory annotations and the broken quick-start save option.
- Package the release as FM3.zip with a short FM3 root folder.
- Check the actual FM3 Plus destination and backup paths before installation or
  update. The mod has longer file paths than the original Disc 2, so a user-data
  folder accepted for Disc 2 can still exceed Windows' path limit with the mod.
- Explain how to choose shorter folders instead of failing partway through
  extraction with a generic write error. Reject an overlong replacement before
  uninstalling an existing mod.



- Reject unsafe paths in mod archives, uninstall state and ISO directory entries.
- Keep an installed mod when a replacement archive fails validation.
- Preserve recovery information when restoring mod files fails.
- Save the mod's uninstall inventory before replacing game files.
- Reject a truncated ISO root instead of reading uninitialised data.
- Avoid truncating CameraSettings.ini after a failed read.
- Require the user-data path for command-line mod operations.
- Generate recompiled sources before the initial game build and document the Disc 1 prerequisite.
- Add launcher regression tests.

### Antivirus scan

Microsoft Defender scanned the release candidate's extracted files and ZIP on
15 October 2026 using security intelligence `1.461.35.0`. The scan reported
**no threats found**. No false positive was reproduced in this check, and the
earlier Forza 2 warnings concern different binaries. The
[Forza 2 v0.3.0 notes](https://github.com/0herculabs/hl360-Forza2/releases/tag/v0.3.0)
report generic or heuristic detections of `rexruntime.dll` in the 0.2.0 package
(14/68 engines on VirusTotal). That historical result does not apply to this package.
This result applies to the tested candidate and scanner; it is not a universal
malware-free certification. The final download's SHA-256 is supplied separately.

### Known limitations

- A mod update interrupted during file replacement can leave a partial update;
  full transactional rollback is not implemented.
- Career mode, local multiplayer and extended gameplay still need testing.
- Full installation of both discs and the real mod was not repeated in this review.
- Frame pacing and first-visit shader stutter remain work in progress.
- Xbox Live is unavailable.
