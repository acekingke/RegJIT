import sys
import os

# 获取当前文件所在目录的父目录（python/），添加到路径
_this_dir = os.path.dirname(os.path.abspath(__file__))
_python_dir = os.path.dirname(_this_dir)
_project_root = os.path.dirname(_python_dir)
sys.path.insert(0, _python_dir)

# 设置动态库搜索路径（macOS/Linux）
if sys.platform == "darwin":
    os.environ["DYLD_LIBRARY_PATH"] = (
        _project_root + ":" + os.environ.get("DYLD_LIBRARY_PATH", "")
    )
else:
    os.environ["LD_LIBRARY_PATH"] = (
        _project_root + ":" + os.environ.get("LD_LIBRARY_PATH", "")
    )

import _regjit


def test_eviction_basic():
    _regjit.set_cache_maxsize(2)

    # compile three distinct patterns (acquire -> release so they are unreferenced)
    _regjit.acquire("pat_a")
    _regjit.release("pat_a")
    _regjit.acquire("pat_b")
    _regjit.release("pat_b")
    _regjit.acquire("pat_c")
    _regjit.release("pat_c")

    size = _regjit.cache_size()
    assert size == 2 or size <= 2, (
        f"expected cache size <= 2 after eviction, got {size}"
    )


def test_no_evict_when_referenced():
    _regjit.set_cache_maxsize(2)

    # Acquire A and keep it referenced
    _regjit.acquire("keep_A")

    # Compile two more patterns and release them so they're evictable
    _regjit.acquire("tmp_B")
    _regjit.release("tmp_B")
    _regjit.acquire("tmp_C")
    _regjit.release("tmp_C")

    # Because keep_A is referenced, eviction should stop and cache may exceed max
    size = _regjit.cache_size()
    assert size >= 3, (
        f"expected cache size >=3 when referenced entry prevents eviction, got {size}"
    )

    # Now release keep_A and trigger eviction by re-setting maxsize
    _regjit.release("keep_A")
    _regjit.set_cache_maxsize(2)
    size2 = _regjit.cache_size()
    assert size2 <= 2, (
        f"expected cache size <=2 after releasing and triggering eviction, got {size2}"
    )


if __name__ == "__main__":
    test_eviction_basic()
    print("eviction_basic ok")
    test_no_evict_when_referenced()
    print("no_evict_when_referenced ok")
