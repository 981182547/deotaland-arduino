# Release tools

Run these commands from the source checkout:

```powershell
./tools/package-library.ps1
python ./tools/check-library-package.py ../dist/deotaland-arduino-0.8.1.zip
```

The package version comes from `library.properties`. Use `-OutputPath` to change
the ZIP destination. The package contains `src`, `examples`, `docs`, the library
metadata, README and licenses. These maintenance tools are not included.

The checker compares every packaged file with the checkout, including licenses,
and checks required examples, a README for each sketch, local Markdown links
and private NimBLE includes.
It rejects unexpected directories, generated products, duplicate entries and
case-colliding paths. It does not require test results or a source manifest.
