# Publishing this component to the ESP Component Registry

How releases of this component reach [components.espressif.com](https://components.espressif.com).

## One-time setup

1. Make the GitHub repository **public** (the Registry only serves public components).
2. Create an API token at <https://components.espressif.com/settings/tokens> with the
   `write:components` scope.
3. Add it as a repository secret named `IDF_COMPONENT_API_TOKEN`
   (GitHub → Settings → Secrets and variables → Actions).

## Release flow (automated)

Releases are fully automated by [.github/workflows/release.yml](../.github/workflows/release.yml).
On every push to `main` it:

1. Reads `version` from `idf_component.yml`.
2. Checks the Registry API; if that version is already published, it warns and skips the upload.
3. Creates the git tag `v<version>` and a GitHub release with generated notes.
4. Uploads the component with `espressif/upload-components-ci-action`.

So publishing a new version is just:

1. Bump `version` in `idf_component.yml` (semantic versioning: `MAJOR.MINOR.PATCH`).
2. Move the relevant `CHANGELOG.md` entries from `[Unreleased]` to the new version.
3. Merge to `main`. The workflow does the rest; the Registry indexes the new version within minutes.

## Manual publishing (fallback)

With [compote](https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/compote_cli.html),
the ESP Component Registry CLI:

```bash
compote registry login                  # authenticate once
compote component pack --name ota-api --version X.Y.Z
compote component upload --namespace pedroluisdionisiofraga --name ota-api
```

Other useful commands:

- `compote component yank` — hide a version from normal dependency resolution (still installable by exact version).
- `compote component delete` — permanently remove a published version (cannot be restored or re-uploaded).
- `compote manifest schema` — inspect the `idf_component.yml` format.

## How consumers install it

```bash
idf.py add-dependency "pedroluisdionisiofraga/ota-api^X.Y.Z"
```

or in the project's `main/idf_component.yml`:

```yaml
dependencies:
  pedroluisdionisiofraga/ota-api: "^X.Y.Z"
```

## Common issues

- **Component not appearing**: repository must be public and the workflow must have run on `main`.
- **Upload rejected**: `idf_component.yml` formatting errors, or version already exists (bump it).
- **Examples fail for consumers**: each directory under `examples/` must build standalone
  (`idf.py build` inside it) — CI enforces this via `build.yml`.
