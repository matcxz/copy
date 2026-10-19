import os
import pathlib
import subprocess
import tempfile
import unittest


PROJECT = pathlib.Path(__file__).resolve().parents[1]
COPY = pathlib.Path(os.environ.get("COPY_BIN", PROJECT / "copy"))


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

    def run_copy_with_umask(self, mask, *arguments):
        return subprocess.run(
            [COPY, *map(str, arguments)],
            capture_output=True,
            text=True,
            check=False,
            preexec_fn=lambda: os.umask(mask),
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

    def test_new_file_modes_follow_source_and_umask(self):
        for source_mode in (0o600, 0o755, 0o444):
            for mask in (0o022, 0o077):
                for resume in (False, True):
                    with self.subTest(
                        source_mode=oct(source_mode), mask=oct(mask), resume=resume
                    ):
                        source = self.root / "mode-source"
                        destination = self.root / "mode-destination"
                        source.write_text("mode test")
                        source.chmod(source_mode)
                        arguments = ["--resume"] if resume else []
                        result = self.run_copy_with_umask(
                            mask, *arguments, source, destination
                        )

                        self.assertEqual(
                            result.returncode, 0,
                            msg=f"stdout={result.stdout}\nstderr={result.stderr}",
                        )
                        self.assertEqual(
                            destination.stat().st_mode & 0o777,
                            source_mode & ~mask,
                        )
                        source.chmod(0o600)
                        source.unlink()
                        destination.unlink()

    def test_new_directory_and_file_modes_follow_source_and_umask(self):
        for mask in (0o022, 0o077):
            for resume in (False, True):
                with self.subTest(mask=oct(mask), resume=resume):
                    source = self.root / "mode-tree"
                    source.mkdir(exist_ok=True)
                    source.chmod(0o700)
                    source_file = source / "private"
                    source_file.write_text("private")
                    source_file.chmod(0o600)
                    destination_parent = self.root / "mode-output"
                    destination_parent.mkdir(exist_ok=True)
                    arguments = ["--recursive"]
                    if resume:
                        arguments.append("--resume")
                    result = self.run_copy_with_umask(
                        mask, *arguments, source, destination_parent
                    )
                    destination = destination_parent / source.name

                    self.assertEqual(
                        result.returncode, 0,
                        msg=f"stdout={result.stdout}\nstderr={result.stderr}",
                    )
                    self.assertEqual(
                        destination.stat().st_mode & 0o777, 0o700 & ~mask
                    )
                    self.assertEqual(
                        (destination / "private").stat().st_mode & 0o777,
                        0o600 & ~mask,
                    )
                    (destination / "private").chmod(0o600)
                    destination.chmod(0o700)
                    (destination / "private").unlink()
                    destination.rmdir()
                    source_file.unlink()

    def test_new_read_only_directory_is_writable_until_copy_finishes(self):
        source = self.root / "read-only-source"
        source.mkdir()
        (source / "payload").write_text("copied through temporary permissions")
        source.chmod(0o555)
        destination_parent = self.root / "read-only-output"
        destination_parent.mkdir()

        result = self.run_copy_with_umask(
            0o022, "--recursive", source, destination_parent
        )
        destination = destination_parent / source.name

        self.assertEqual(
            result.returncode, 0, msg=f"stdout={result.stdout}\nstderr={result.stderr}"
        )
        self.assertTrue((destination / "payload").is_file())
        self.assertEqual(destination.stat().st_mode & 0o777, 0o555)
        destination.chmod(0o700)
        (destination / "payload").unlink()
        destination.rmdir()
        source.chmod(0o700)

    def test_overwriting_existing_file_keeps_its_mode(self):
        source = self.root / "mode-overwrite-source"
        destination = self.root / "mode-overwrite-destination"
        source.write_text("new content")
        source.chmod(0o600)
        destination.write_text("old content")
        destination.chmod(0o640)

        result = self.run_copy_with_umask(0o077, source, destination)

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertEqual(destination.stat().st_mode & 0o777, 0o640)
        self.assertEqual(destination.read_text(), "new content")

    def test_overwriting_existing_directory_keeps_modes_without_preserve(self):
        source = self.root / "existing-mode-source"
        source.mkdir()
        (source / "payload").write_text("new content")
        source.chmod(0o700)
        destination_parent = self.root / "existing-mode-output"
        destination_parent.mkdir()
        destination = destination_parent / source.name
        destination.mkdir()
        (destination / "payload").write_text("old content")
        destination.chmod(0o750)
        (destination / "payload").chmod(0o640)

        result = self.run_copy_with_umask(
            0o077, "--recursive", source, destination_parent
        )

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertEqual(destination.stat().st_mode & 0o777, 0o750)
        self.assertEqual((destination / "payload").stat().st_mode & 0o777, 0o640)
        self.assertEqual((destination / "payload").read_text(), "new content")

    def test_preserve_keeps_exact_source_modes_with_restrictive_umask(self):
        source = self.root / "preserve-mode-source"
        source.write_text("preserved")
        source.chmod(0o755)
        destination = self.root / "preserve-mode-destination"

        result = self.run_copy_with_umask(0o077, "--preserve", source, destination)

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertEqual(destination.stat().st_mode & 0o777, 0o755)

        source_dir = self.root / "preserve-mode-tree"
        source_dir.mkdir()
        source_file = source_dir / "private"
        source_file.write_text("private")
        source_dir.chmod(0o700)
        source_file.chmod(0o600)
        destination_parent = self.root / "preserve-mode-tree-output"
        destination_parent.mkdir()

        result = self.run_copy_with_umask(
            0o077, "--preserve", "--recursive", source_dir, destination_parent
        )
        copied_dir = destination_parent / source_dir.name

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertEqual(copied_dir.stat().st_mode & 0o777, 0o700)
        self.assertEqual((copied_dir / "private").stat().st_mode & 0o777, 0o600)

    def test_dry_run_file_copy_does_not_create_or_modify_destination(self):
        destination = self.root / "planned-copy.txt"
        result = self.run_copy("--dry-run", self.source / "keep.txt", destination)

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("Would copy", result.stdout)
        self.assertFalse(destination.exists())

    def test_dry_run_recursive_copy_lists_files_and_links_without_changes(self):
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "payload.txt").write_text("nested content")
        (self.source / "link").symlink_to("keep.txt")
        destination = self.root / "planned-tree"

        result = self.run_copy("--dry-run", "--recursive", self.source, destination)

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("Would create directory", result.stdout)
        self.assertIn(str(destination / "nested" / "payload.txt"), result.stdout)
        self.assertIn("Would copy symbolic link", result.stdout)
        self.assertFalse(destination.exists())
        self.assertEqual((self.source / "keep.txt").read_text(), "existing content")

    def test_dry_run_no_clobber_reports_existing_files_and_keeps_them(self):
        destination_dir = self.root / "existing-destination"
        destination_dir.mkdir()
        destination_file = destination_dir / "d"
        destination_file.mkdir()
        existing_file = destination_file / "keep.txt"
        existing_file.write_text("do not replace")

        result = self.run_copy(
            "--dry-run", "--no-clobber", "--recursive",
            self.source, destination_dir
        )

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("Would skip existing destination", result.stdout)
        self.assertEqual(existing_file.read_text(), "do not replace")

    def test_dry_run_no_clobber_skips_existing_directory_destination(self):
        destination = self.root / "existing-directory"
        destination.mkdir()
        existing_file = destination / "keep.txt"
        existing_file.write_text("do not replace")

        result = self.run_copy(
            "--dry-run", "--no-clobber", self.source / "keep.txt", destination
        )

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("Would skip existing destination", result.stdout)
        self.assertEqual(existing_file.read_text(), "do not replace")

    def test_dry_run_refuses_same_file_destination(self):
        source = self.source / "keep.txt"

        result = self.run_copy("--dry-run", source, source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same file", result.stderr)
        self.assertEqual(source.read_text(), "existing content")

    def test_dry_run_with_resume_does_not_create_partial_file(self):
        destination = self.root / "resumable-target"

        result = self.run_copy("--dry-run", "--resume", self.source / "keep.txt",
                               destination)

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("Would copy", result.stdout)
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.root.glob(".copy-resume-*")))

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
