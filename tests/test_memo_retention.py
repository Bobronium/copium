"""
Memo table retention / shrink policy tests.

TSS memo is reused across deepcopy() calls. After each call, the table is cleared.
If table.size exceeded MEMO_RETAIN_MAX_SLOTS (2^17), it shrinks to 2^13 slots.

Observability challenge: capturing the memo via __deepcopy__ bumps refcount,
which diverts cleanup_memo away from the reset() path. So we can't inspect
the memo mid-shrink.

Strategy: two-phase observation.
  Phase 1 — deepcopy a large graph WITHOUT capture. Normal cleanup runs
            reset(), which clears and conditionally shrinks the table.
  Phase 2 — deepcopy a tiny graph WITH capture. The spy sees the memo
            AFTER phase 1's shrink already happened. We read table.size
            via ctypes using PyMemoObject's #[repr(C)] layout.
"""

import ctypes
import gc
import struct
import sys

import pytest

import copium

MEMO_RETAIN_MAX_SLOTS = 1 << 17
EXPECTED_SHRINK_SIZE = 1 << 13

_PTR = struct.calcsize("P")

# PyMemoObject is #[repr(C)]:
#   PyObject      ob_refcnt  ob_type
#   MemoTable     slots      size     used     filled
#   KeepaliveVec  (Vec internals)
#   UndoLog       (Vec internals)
#   dict_proxy
#
# MemoTable fields are all pointer-sized — no padding, no reorder benefit.
_OFF_REFCNT = 0
_OFF_TABLE_SLOTS = 2 * _PTR
_OFF_TABLE_SIZE = 3 * _PTR
_OFF_TABLE_USED = 4 * _PTR


def _usize_at(addr, offset):
    return ctypes.c_size_t.from_address(addr + offset).value


def _ssize_at(addr, offset):
    return ctypes.c_ssize_t.from_address(addr + offset).value


def _check_offset_model(memo):
    """Return None if layout matches, or a reason string to skip."""
    addr = id(memo)

    raw_rc = _ssize_at(addr, _OFF_REFCNT)
    expect_rc = sys.getrefcount(memo) - 1
    if abs(raw_rc - expect_rc) > 1:
        return f"refcount: read {raw_rc}, expected ~{expect_rc}"

    tsize = _usize_at(addr, _OFF_TABLE_SIZE)
    if tsize != 0 and (tsize & (tsize - 1)) != 0:
        return f"table_size {tsize} is not a power of two"

    tused = _usize_at(addr, _OFF_TABLE_USED)
    mlen = len(memo)
    if not (tused <= mlen <= tused + 1):
        return f"table_used={tused} vs len(memo)={mlen}"

    return None


class _MemoIdSpy:
    """Records memo id without preventing TSS reuse."""

    def __deepcopy__(self, memo):
        clone = _MemoIdSpy()
        clone.memo_id = id(memo)
        return clone


class _MemoCapture:
    """Holds a reference for post-deepcopy ctypes inspection."""

    def __deepcopy__(self, memo):
        clone = _MemoCapture()
        clone.memo = memo
        return clone


def _capture_tss_after(setup):
    """Run setup (no capture, normal reset), then capture the TSS memo."""
    setup()
    gc.collect()

    spy = _MemoCapture()
    result = copium.deepcopy([spy])
    memo = result[0].memo

    reason = _check_offset_model(memo)
    if reason:
        pytest.skip(f"ctypes offset model invalid: {reason}")

    return memo


@pytest.fixture(autouse=True)
def _gc():
    gc.collect()
    yield
    gc.collect()


needs_64bit = pytest.mark.skipif(_PTR != 8, reason="64-bit layout assumed")


@needs_64bit
class TestTableRetention:
    def test_shrinks_after_exceeding_threshold(self):
        N = 200_000

        def setup():
            copium.deepcopy([[] for _ in range(N)])

        memo = _capture_tss_after(setup)
        table_size = _usize_at(id(memo), _OFF_TABLE_SIZE)

        assert table_size == EXPECTED_SHRINK_SIZE, (
            f"after {N} entries: table_size={table_size}, expected {EXPECTED_SHRINK_SIZE}"
        )

    def test_not_shrunk_at_exact_boundary(self):
        """131072 slots is the threshold; condition is strict >, so no shrink."""
        N = 50_000  # grows table to exactly 131072 slots

        def setup():
            copium.deepcopy([[] for _ in range(N)])

        memo = _capture_tss_after(setup)
        table_size = _usize_at(id(memo), _OFF_TABLE_SIZE)

        assert table_size == MEMO_RETAIN_MAX_SLOTS, (
            f"after {N} entries: table_size={table_size}, expected {MEMO_RETAIN_MAX_SLOTS}"
        )

    def test_not_shrunk_below_threshold(self):
        N = 1_000

        def setup():
            copium.deepcopy([[] for _ in range(N)])

        memo = _capture_tss_after(setup)
        table_size = _usize_at(id(memo), _OFF_TABLE_SIZE)

        assert 8 <= table_size <= MEMO_RETAIN_MAX_SLOTS

    def test_shrink_is_idempotent(self):
        """Two consecutive large copies both shrink; second starts from shrunk state."""
        N = 200_000

        def setup():
            copium.deepcopy([[] for _ in range(N)])
            gc.collect()
            copium.deepcopy([[] for _ in range(N)])

        memo = _capture_tss_after(setup)
        table_size = _usize_at(id(memo), _OFF_TABLE_SIZE)

        assert table_size == EXPECTED_SHRINK_SIZE


class TestTSSLifecycle:
    def test_reused_when_not_borrowed(self):
        s1 = _MemoIdSpy()
        r1 = copium.deepcopy([s1])
        id1 = r1[0].memo_id
        del r1
        gc.collect()

        s2 = _MemoIdSpy()
        r2 = copium.deepcopy([s2])
        id2 = r2[0].memo_id
        del r2

        assert id1 == id2

    def test_fresh_allocation_when_borrowed(self):
        cap = _MemoCapture()
        r1 = copium.deepcopy([cap])
        held = r1[0].memo

        s2 = _MemoIdSpy()
        r2 = copium.deepcopy([s2])
        id2 = r2[0].memo_id

        assert id(held) != id2
        del held, r1, r2

    def test_cleared_between_calls(self):
        copium.deepcopy([[] for _ in range(1000)])
        gc.collect()

        observed = None

        class LenProbe:
            def __deepcopy__(self, memo):
                nonlocal observed
                observed = len(memo)
                return LenProbe()

        copium.deepcopy([LenProbe()])

        assert observed is not None
        assert observed < 10, f"stale entries from previous call: len={observed}"
