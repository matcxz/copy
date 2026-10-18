import fcntl
import os
import pathlib
import stat
import subprocess
import time
import tempfile
import unittest


PROJECT = pathlib.Path(__file__).resolve().parents[1]
COPY = PROJECT / "copy"


def resume_path(destination):
    value = 14695981039346656037
    for byte in os.fsencode(destination):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return destination.parent / f".copy-resume-{value:016x}"


class ResumeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.slow_write_dir = tempfile.TemporaryDirectory(
            prefix="copy-slow-write-"
        )
        cls.slow_write_library = pathlib.Path(cls.slow_write_dir.name) / "slow-write.so"
        subprocess.run(
            [
                "cc",
                "-shared",
                "-fPIC",
                "-O2",
                PROJECT / "tests" / "slow_write.c",
                "-o",
                cls.slow_write_library,
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.slow_write_dir.cleanup()

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="copy-resume-test-")
        self.root = pathlib.Path(self.temp_dir.name)
        self.source = self.root / "source.bin"
        self.destination = self.root / "destination.bin"
        self.content = bytes(range(256)) * 8192
        self.source.write_bytes(self.content)

    def tearDown(self):
        self.temp_dir.cleanup()

    def run_copy(self, *arguments, expected_status=0):
        result = subprocess.run(
            [COPY, *map(str, arguments)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            expected_status,
            msg=f"stdout={result.stdout}\nstderr={result.stderr}",
        )
        return result

    def test_resumes_matching_partial_file_and_publishes_verified_result(self):
        partial = resume_path(self.destination)
        partial.write_bytes(self.content[:700_000])

        self.run_copy("--resume", self.source, self.destination)

        self.assertEqual(self.destination.read_bytes(), self.content)
        self.assertFalse(partial.exists())

    def test_resumes_files_inside_a_recursive_copy(self):
        source_dir = self.root / "source-dir"
        source_file = source_dir / "nested" / "payload"
        source_file.parent.mkdir(parents=True)
        source_file.write_bytes(self.content)

        destination_dir = self.root / "destination-dir"
        destination_file = destination_dir / "source-dir" / "nested" / "payload"
        destination_file.parent.mkdir(parents=True)
        partial = resume_path(destination_file)
        partial.write_bytes(self.content[:900_000])

        self.run_copy("--recursive", "--resume", source_dir, destination_dir)

        self.assertEqual(destination_file.read_bytes(), self.content)
        self.assertFalse(partial.exists())

    def test_restarts_when_partial_data_does_not_match_source(self):
        partial = resume_path(self.destination)
        partial.write_bytes(b"stale partial data")

        self.run_copy("--resume", self.source, self.destination)

        self.assertEqual(self.destination.read_bytes(), self.content)
        self.assertFalse(partial.exists())

    def test_restarts_when_source_changes_after_interruption(self):
        partial = resume_path(self.destination)
        partial.write_bytes(self.content[:700_000])
        changed_content = bytearray(self.content)
        changed_content[100] ^= 0xFF
        self.source.write_bytes(changed_content)

        self.run_copy("--resume", self.source, self.destination)

        self.assertEqual(self.destination.read_bytes(), changed_content)
        self.assertFalse(partial.exists())

    def test_skips_already_complete_files_in_recursive_copy(self):
        source_dir = self.root / "source-dir"
        source_file = source_dir / "nested" / "payload"
        source_file.parent.mkdir(parents=True)
        source_file.write_bytes(self.content)

        destination_dir = self.root / "destination-dir"
        destination_file = destination_dir / "source-dir" / "nested" / "payload"
        destination_file.parent.mkdir(parents=True)
        destination_file.write_bytes(self.content)
        original_inode = destination_file.stat().st_ino

        result = self.run_copy(
            "--recursive", "--resume", "--verbose", source_dir, destination_dir
        )

        self.assertEqual(destination_file.stat().st_ino, original_inode)
        self.assertIn("(already complete)", result.stdout)

    def test_sigkill_interruption_keeps_destination_and_resumes(self):
        content = bytes(range(256)) * (256 * 1024)
        self.source.write_bytes(content)
        self.destination.write_bytes(b"previous destination")
        partial = resume_path(self.destination)
        environment = os.environ.copy()
        environment["LD_PRELOAD"] = str(self.slow_write_library)
        process = subprocess.Popen(
            [COPY, "--resume", self.source, self.destination],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 15
        try:
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    self.fail("copy completed before it could be interrupted")
                if partial.exists() and partial.stat().st_size >= 128 * 1024:
                    break
                time.sleep(0.005)
            else:
                self.fail("copy did not create a resumable partial file")

            self.assertEqual(self.destination.read_bytes(), b"previous destination")
            self.assertLess(partial.stat().st_size, len(content))
            process.kill()
            process.wait(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

        self.assertEqual(self.destination.read_bytes(), b"previous destination")
        self.assertGreater(partial.stat().st_size, 0)

        self.run_copy("--resume", self.source, self.destination)

        self.assertEqual(self.destination.read_bytes(), content)
        self.assertFalse(partial.exists())

    def test_keeps_existing_destination_if_resume_file_is_unsafe(self):
        self.destination.write_bytes(b"keep the published destination")
        partial = resume_path(self.destination)
        partial.mkdir()

        result = self.run_copy(
            "--resume", self.source, self.destination, expected_status=1
        )

        self.assertIn("resume file", result.stderr)
        self.assertEqual(
            self.destination.read_bytes(), b"keep the published destination"
        )

    def test_refuses_a_resume_file_locked_by_another_copy(self):
        partial = resume_path(self.destination)
        partial.write_bytes(self.content[:500_000])
        with partial.open("rb") as locked_file:
            fcntl.flock(locked_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_copy(
                "--resume", self.source, self.destination, expected_status=1
            )

        self.assertIn("already in use", result.stderr)
        self.assertFalse(self.destination.exists())

    def test_refuses_hard_linked_resume_files(self):
        partial = resume_path(self.destination)
        unrelated = self.root / "unrelated"
        unrelated.write_bytes(b"do not overwrite this")
        os.link(unrelated, partial)

        result = self.run_copy(
            "--resume", self.source, self.destination, expected_status=1
        )

        self.assertIn("unsafe resume file", result.stderr)
        self.assertEqual(unrelated.read_bytes(), b"do not overwrite this")
        self.assertFalse(self.destination.exists())

    def test_no_clobber_does_not_replace_existing_destination(self):
        self.destination.write_bytes(b"keep this")

        self.run_copy("--resume", "--no-clobber", self.source, self.destination)

        self.assertEqual(self.destination.read_bytes(), b"keep this")
        self.assertFalse(resume_path(self.destination).exists())

    def test_preserves_metadata_when_requested(self):
        os.chmod(self.source, 0o640)
        timestamp_ns = 1_700_000_000_000_000_000
        os.utime(self.source, ns=(timestamp_ns, timestamp_ns))

        self.run_copy("--resume", "--preserve", self.source, self.destination)

        self.assertEqual(self.destination.read_bytes(), self.content)
        self.assertEqual(stat.S_IMODE(self.destination.stat().st_mode), 0o640)
        self.assertEqual(self.destination.stat().st_mtime_ns, timestamp_ns)

    def test_refuses_symlink_destination_without_replacing_it(self):
        existing = self.root / "existing.bin"
        existing.write_bytes(b"keep target")
        self.destination.symlink_to(existing.name)

        self.run_copy(
            "--resume", self.source, self.destination, expected_status=1
        )

        self.assertTrue(self.destination.is_symlink())
        self.assertEqual(existing.read_bytes(), b"keep target")

    def test_resume_option_is_available_in_help(self):
        result = self.run_copy("--help")
        self.assertIn("--resume", result.stdout)
        self.assertIn("verify before publishing", result.stdout)


if __name__ == "__main__":
    unittest.main()
