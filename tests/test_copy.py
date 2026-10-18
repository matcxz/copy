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

    def test_verifies_empty_small_and_large_files(self):
        for size in (0, 37, 256 * 1024 + 19):
            with self.subTest(size=size):
                source = self.root / f"verify-source-{size}.bin"
                destination = self.root / f"verify-destination-{size}.bin"
                content = bytes(index % 251 for index in range(size))
                source.write_bytes(content)

                result = self.run_copy("--verify", source, destination)

                self.assertEqual(
                    result.returncode,
                    0,
                    msg=f"stdout={result.stdout}\nstderr={result.stderr}",
                )
                self.assertEqual(destination.read_bytes(), content)

    def test_preserves_sparse_files_for_normal_and_resumed_copies(self):
        file_size = 64 * 1024 * 1024
        source = self.root / "sparse-source.bin"
        with source.open("wb") as stream:
            for offset, content in (
                (0, b"beginning" * 128),
                (file_size // 2, b"middle" * 128),
                (file_size - 4096, b"ending" * 128),
            ):
                stream.seek(offset)
                stream.write(content)
            stream.truncate(file_size)

        self.assertLess(source.stat().st_blocks, file_size // 512 // 4)

        for mode in ("normal", "resume"):
            with self.subTest(mode=mode):
                destination = self.root / f"sparse-{mode}.bin"
                arguments = ["--resume"] if mode == "resume" else []
                result = self.run_copy(*arguments, source, destination)

                self.assertEqual(
                    result.returncode,
                    0,
                    msg=f"stdout={result.stdout}\nstderr={result.stderr}",
                )
                self.assertEqual(destination.stat().st_size, file_size)
                self.assertLess(destination.stat().st_blocks, file_size // 512 // 4)
                with source.open("rb") as source_stream, destination.open(
                    "rb"
                ) as destination_stream:
                    while True:
                        source_chunk = source_stream.read(1024 * 1024)
                        destination_chunk = destination_stream.read(1024 * 1024)
                        self.assertEqual(destination_chunk, source_chunk)
                        if not source_chunk:
                            break


if __name__ == "__main__":
    unittest.main()
