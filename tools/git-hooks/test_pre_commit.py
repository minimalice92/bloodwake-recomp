import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HOOK = Path(__file__).with_name("pre-commit")


@unittest.skipUnless(os.name == "posix", "executable mode is POSIX-specific")
class PreCommitHookTests(unittest.TestCase):
    def test_git_invokes_hook_and_blocks_disc_image(self):
        self.assertTrue(os.access(HOOK, os.X_OK))

        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)

            def git(*args):
                return subprocess.run(
                    ["git", "-C", str(repo), *args],
                    capture_output=True,
                    text=True,
                )

            self.assertEqual(git("init", "--quiet").returncode, 0)
            self.assertEqual(
                git("config", "core.hooksPath", str(HOOK.parent)).returncode, 0)
            self.assertEqual(git("config", "user.name", "Hook Test").returncode, 0)
            self.assertEqual(
                git("config", "user.email", "hook-test@example.invalid").returncode,
                0,
            )

            (repo / "safe.txt").write_text("synthetic fixture\n", encoding="utf-8")
            self.assertEqual(git("add", "safe.txt").returncode, 0)
            safe_commit = git("commit", "-m", "Safe fixture")
            self.assertEqual(safe_commit.returncode, 0, safe_commit.stderr)

            (repo / "dummy.iso").write_bytes(b"synthetic fixture")
            self.assertEqual(git("add", "dummy.iso").returncode, 0)
            blocked_commit = git("commit", "-m", "Blocked fixture")
            self.assertNotEqual(blocked_commit.returncode, 0)
            self.assertIn("pre-commit: BLOCKED dummy.iso", blocked_commit.stderr)


if __name__ == "__main__":
    unittest.main()
