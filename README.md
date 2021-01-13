[![License](https://img.shields.io/badge/license-Python%20Software%20Foundation-lightgray.svg)](https://opensource.org/licenses/Python-2.0)
[![Build Status](https://travis-ci.com/leejeonghun/import7z.svg?branch=master)](https://travis-ci.com/leejeonghun/import7z)
[![codecov](https://codecov.io/gh/leejeonghun/import7z/branch/master/graph/badge.svg?token=DBEP9B3EAJ)](https://codecov.io/gh/leejeonghun/import7z)

# import7z

This module is a modified version of zipimport to import Python modules and packages from 7z-format archives instead ZIP-format.

## Usage

```python
import import7z

sys.path.insert(0, 'example.7z')
sys.path_hooks.insert(0, import7z.importer7z)

import some_module_in_7z
```

## License

It's Python Software Foundation License cause it used zipimport.c from CPython 3.6.
LZMA SDK is public domain.
