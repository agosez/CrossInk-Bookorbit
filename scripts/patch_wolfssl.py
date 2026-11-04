from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
/* Arduino-wolfSSL turns on DEBUG_WOLFSSL, which compiles every WOLFSSL_MSG and
   WOLFSSL_ENTER trace string into flash. Keep it only for FREEINK_WOLFSSL_DEBUG. */
#ifndef FREEINK_WOLFSSL_DEBUG
#undef DEBUG_WOLFSSL
#endif
"""


def patch_user_settings(path: Path) -> None:
    original_text = path.read_text()
    text = original_text
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    patched_text = text.rstrip() + OVERRIDES + "\n"
    if original_text == patched_text:
        return
    path.write_text(patched_text)
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


# wolfSSL grows its input buffer to fit each record larger than the static
# buffer, then frees it again as soon as the record is consumed. A server that
# ignores max_fragment_length sends 16 KB records, so a large download on a
# PSRAM-less C3 needs a fresh ~17 KB contiguous block for every record, and
# fails with MEMORY_E once fragmentation leaves none. Keep the grown buffer
# until the connection is freed: one allocation per connection, failing at the
# first record rather than megabytes into the transfer.
SHRINK_MARKER = "/* CrossInk: keep a grown input buffer until the connection is freed */"
SHRINK_ORIGINAL = """    if (!forcedFree && (usedLength > STATIC_BUFFER_LEN ||
            ssl->buffers.clearOutputBuffer.length > 0))
        return;
"""
# The source lives in PlatformIO's dependency cache. Keep the original branch
# available so reverting this script also disables retention in an already
# patched dependency, without requiring users to delete their package cache.
SHRINK_PATCHED = f"""    {SHRINK_MARKER}
#if defined(FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER)
    if (!forcedFree)
        return;
#else
{SHRINK_ORIGINAL}#endif
"""
env.Append(CPPDEFINES=["FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER"])


# A retained buffer still has to grow when a larger record arrives, and it used
# to hold its old block while allocating the new one. Go's crypto/tls (Caddy,
# Traefik...) ignores max_fragment_length and grows its records 1186 bytes at a
# time up to 16 KB, so a transfer made a dozen such allocations, each larger than
# any hole the previous ones left, and the last failed about 100 KB in, on a heap
# that had 70 KB in one block before the handshake (issue #96). Release the old
# block first when nothing has to be carried over, and let the app ask for a full
# record the first time, while that block is still whole: it knows which requests
# are worth the memory (HttpDownloader's DownloadOptions::largeBody).
GROW_MARKER = "/* CrossInk: reach a full record in one input buffer allocation */"
GROW_ORIGINAL = """    tmp = (byte*)XMALLOC(size + usedLength + align,
                             ssl->heap, DYNAMIC_TYPE_IN_BUFFER);
    WOLFSSL_MSG("growing input buffer");
"""
GROW_PATCHED = f"""    {GROW_MARKER}
#if defined(FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER)
    tmp = NULL;
    /* With nothing to carry over, the old block can merge with its neighbours. */
    if (usedLength == 0 && ssl->buffers.inputBuffer.dynamicFlag)
        ShrinkInputBuffer(ssl, FORCED_FREE);
    if (size + usedLength < FREEINK_WOLFSSL_FULL_RECORD_BUFFER) {{
        extern int freeinkWolfsslWantsFullRecordBuffer(void);
        if (freeinkWolfsslWantsFullRecordBuffer()) {{
            tmp = (byte*)XMALLOC(FREEINK_WOLFSSL_FULL_RECORD_BUFFER + align,
                                 ssl->heap, DYNAMIC_TYPE_IN_BUFFER);
            if (tmp != NULL)
                size = FREEINK_WOLFSSL_FULL_RECORD_BUFFER - usedLength;
        }}
    }}
    if (tmp == NULL)
#endif
{GROW_ORIGINAL}"""
# MAX_RECORD_SIZE plus TLS 1.3's largest record expansion (RFC 8446, section 5.2).
env.Append(CPPDEFINES=[("FREEINK_WOLFSSL_FULL_RECORD_BUFFER", 16640)])


def patch_once(path: Path, marker: str, original: str, patched: str, description: str) -> None:
    text = path.read_text()
    if marker in text:
        if text.count(patched) != 1:
            raise RuntimeError(f"Corrupt wolfSSL {description} patch in {path}")
        return
    if text.count(original) != 1:
        raise RuntimeError(f"Unsupported wolfSSL {description} code in {path}; refusing an unverified patch")
    path.write_text(text.replace(original, patched, 1))
    print(f"Patched wolfSSL {description}: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)

for internal in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/src/internal.c"):
    patch_once(internal, SHRINK_MARKER, SHRINK_ORIGINAL, SHRINK_PATCHED, "input buffer shrink")
    patch_once(internal, GROW_MARKER, GROW_ORIGINAL, GROW_PATCHED, "input buffer growth")
