# Changelog

## Unreleased

- **Failure reports end to end** ([docs/ERRORS.md](docs/ERRORS.md)). A failed
  run ends with a `RUN_FAILED` message naming the code (`NO_DATA`,
  `UPSTREAM_HTTP`, …), the step, the upstream URL / HTTP status / response
  excerpt, the input and the processor version. The viewer SDK shows an error
  card with the technical details and a "Report on GitHub" button that
  pre-fills a [run-failure issue](.github/ISSUE_TEMPLATE/run-failure.yml).
- **Tile discovery follows IGN's new LiDAR HD WFS layer**
  (`IGNF_LIDAR-HD_METADONNEE:metadata`).
- An IGN tile index that lists no tile yet is no longer cached, so a retry
  picks up newly published tiles.
- Location picker shows geocoder failures instead of an empty list.
- **Extension seams for hosted deployments** ([docs/EXTENSIONS.md](docs/EXTENSIONS.md)):
  a processor extension linked with `-DDIORAMAP_EXTENSION_DIR` can authenticate
  connections (credential + Origin), restrict them to public artifacts
  (`FORBIDDEN_ARTIFACT` otherwise) and receive a report for every finished
  run, successful or not. The viewer SDK takes `auth: { param, value }` and
  `plugins`, and now forwards `locale`, `issueRepo` and `reportIssues`.

## 0.1.0 (2026-11-22, research release)

First public release of the Dioramap Lab engine, steps, pipelines and browser apps.
Development history before this release is not included in the public repository.
