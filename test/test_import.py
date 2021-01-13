import os
import sys
import unittest
import import7z


class Unittest(unittest.TestCase):
    def __init__(self, *args, **kwargs):
        unittest.TestCase.__init__(self, *args, **kwargs)
        cwd = os.path.dirname(os.path.abspath(__file__))
        path7z = os.path.join(cwd, 'test.7z')
        sys.path.insert(0, path7z)
        sys.path_hooks.insert(0, import7z.importer7z)
        sys.path_importer_cache.clear()

    def test_import_module(self):
        import module1
        self.assertTrue(module1.imported)

    def test_import_module_in_package(self):
        import pak.module2
        self.assertTrue(pak.module2.imported)


if __name__ == "__main__":
    unittest.main()
