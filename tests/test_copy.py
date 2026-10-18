import pathlib
import subprocess
import tempfile
import unittest


PROJECT = pathlib.Path(__file__).resolve().parents[1]
COPY = PROJECT / "copy"


class DirectoryCopyTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="copy-directory-test-")
        self.root = pathlib.Path(self.temp_dir.name)
        self.source = self.root / "d"
        self.source.mkdir()
        (self.source / "keep.txt").write_text("existing content")

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_copy(self, *arguments):
        return subprocess.run(
            [COPY, *map(str, arguments)],
            capture_output=True,
            text=True,
            check=False,
        )

    def assert_refused_without_changes(self, destination):
        result = self.run_copy("--recursive", self.source, destination)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot copy directory", result.stderr)
        self.assertIn("into itself", result.stderr)
        self.assertEqual(
            sorted(path.relative_to(self.source) for path in self.source.rglob("*")),
            [pathlib.Path("keep.txt")],
        )
        self.assertEqual((self.source / "keep.txt").read_text(), "existing content")

    def test_refuses_copy_into_nonexistent_child(self):
        self.assert_refused_without_changes(self.source / "sub")
        self.assertFalse((self.source / "sub").exists())

    def test_refuses_existing_destination_that_is_source_directory(self):
        self.assert_refused_without_changes(self.root)


if __name__ == "__main__":
    unittest.main()
