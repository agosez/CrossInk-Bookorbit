"""Focused, host-only checks for the wolfSSL build patches."""
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

class WolfSslPatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        class Env:
            def subst(inner, value): return str(self.root)
            def Append(inner, **kwargs): pass
        self.module = runpy.run_path(str(Path(__file__).with_name("patch_wolfssl.py")),
                                    init_globals={"Import": lambda _: None, "env": Env()})

    def test_shrink_patch_is_exact_and_idempotent(self):
        path = self.root / "internal.c"
        path.write_text("before\n" + self.module["SHRINK_ORIGINAL"] + "after\n")
        self.module["patch_input_buffer_shrink"](path)
        once = path.read_bytes()
        self.module["patch_input_buffer_shrink"](path)
        self.assertEqual(once, path.read_bytes())
        self.assertIn(b"if (!forcedFree)", once)

    def test_reverting_build_flag_restores_original_guard(self):
        path = self.root / "guard.c"
        path.write_text(self.module["SHRINK_PATCHED"])
        original = subprocess.check_output(["cc", "-E", "-P", "-x", "c", str(path)], text=True)
        retained = subprocess.check_output(["cc", "-E", "-P", "-x", "c", "-DFREEINK_WOLFSSL_RETAIN_INPUT_BUFFER", str(path)], text=True)
        self.assertIn("usedLength > STATIC_BUFFER_LEN", original)
        self.assertNotIn("usedLength > STATIC_BUFFER_LEN", retained)
        self.assertIn("if (!forcedFree)", retained)

    def test_unknown_source_is_rejected(self):
        path = self.root / "internal.c"
        path.write_text("upstream changed")
        with self.assertRaises(RuntimeError): self.module["patch_input_buffer_shrink"](path)
        self.assertEqual(path.read_text(), "upstream changed")

    def test_growth_patch_is_exact_and_idempotent(self):
        path = self.root / "internal.c"
        path.write_text("before\n" + self.module["GROW_ORIGINAL"] + "after\n")
        self.module["patch_input_buffer_growth"](path)
        once = path.read_bytes()
        self.module["patch_input_buffer_growth"](path)
        self.assertEqual(once, path.read_bytes())
        self.assertIn(b"ShrinkInputBuffer(ssl, FORCED_FREE)", once)

    def test_growth_unknown_source_is_rejected(self):
        path = self.root / "internal.c"
        path.write_text("upstream changed")
        with self.assertRaises(RuntimeError): self.module["patch_input_buffer_growth"](path)
        self.assertEqual(path.read_text(), "upstream changed")

    def test_growth_patch_releases_buffer_only_when_safe(self):
        path = self.root / "guard.c"
        path.write_text(self.module["GROW_PATCHED"])
        plain = subprocess.check_output(["cc", "-E", "-P", "-x", "c", str(path)], text=True)
        released = subprocess.check_output(
            ["cc", "-E", "-P", "-x", "c", "-DFREEINK_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER", str(path)], text=True)
        self.assertNotIn("ShrinkInputBuffer", plain)
        self.assertIn("ssl->buffers.inputBuffer.dynamicFlag && usedLength == 0", released)
        self.assertIn("ShrinkInputBuffer(ssl, FORCED_FREE)", released)

    def test_growth_patch_frees_old_buffer_before_allocating_new_one(self):
        # Reproduces GrowInputBuffer's allocator call with a budget too small to
        # hold the old (consumed) and new buffers at once, proving the patched
        # branch frees the old allocation first instead of needing both live.
        import re
        grown_call = re.sub(
            r"XMALLOC\(size \+ usedLength \+ align,\s*ssl->heap, DYNAMIC_TYPE_IN_BUFFER\)",
            "alloc(size + usedLength + align)", self.module["GROW_PATCHED"])
        harness = self.root / "check.c"
        harness.write_text(r"""
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
#define FORCED_FREE 1
#define DYNAMIC_TYPE_IN_BUFFER 0
#define WOLFSSL_MSG(x) ((void)0)
static size_t live, budget, old_size;
static int shrink_called;
typedef struct { byte *buffer; size_t length, idx; int dynamicFlag; } InBuf;
typedef struct { InBuf inputBuffer; struct { size_t length; } clearOutputBuffer; } Buffers;
typedef struct { Buffers buffers; void *heap; } SSL;
static void ShrinkInputBuffer(SSL *ssl, int forcedFree) {
    (void)forcedFree;
    shrink_called = 1;
    live -= old_size;
    free(ssl->buffers.inputBuffer.buffer);
    ssl->buffers.inputBuffer.dynamicFlag = 0;
}
static void *alloc(size_t size) {
    if (size > budget - live) return NULL;
    live += size;
    return malloc(size);
}
int main(void) {
    SSL ssl_storage = {0};
    SSL *ssl = &ssl_storage;
    old_size = 8192;
    budget = 20000; /* enough for the new 16 KB buffer alone, not for old+new at once */
    live = old_size;
    ssl->buffers.inputBuffer.buffer = malloc(old_size);
    ssl->buffers.inputBuffer.dynamicFlag = 1;
    ssl->buffers.inputBuffer.idx = ssl->buffers.inputBuffer.length = 0;
    int usedLength = 0, size = 16384, align = 0;
    byte *tmp = NULL;
""" + grown_call + r"""
    assert(shrink_called);
    assert(tmp != NULL);
    free(tmp);
    return 0;
}
""")
        binary = self.root / "check"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                         "-fsanitize=address,undefined",
                         "-DFREEINK_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER",
                         str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__": unittest.main()
