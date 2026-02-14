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

from _regjit import Regex


def test_basic_match():
    """测试基本的正则匹配功能"""
    r = Regex("ab*c")
    assert r.match("ac"), "'ac' should match 'ab*c'"
    assert r.match("abbc"), "'abbc' should match 'ab*c'"
    assert not r.match("ab"), "'ab' should not match 'ab*c'"


def test_anchors():
    """测试锚点功能"""
    # ^ 行首锚点
    r_start = Regex("^abc")
    assert r_start.match("abc"), "'^abc' should match 'abc'"
    assert r_start.match("abcdef"), "'^abc' should match 'abcdef'"
    assert not r_start.match("xabc"), "'^abc' should not match 'xabc'"

    # $ 行尾锚点
    r_end = Regex("abc$")
    assert r_end.match("abc"), "'abc$' should match 'abc'"
    # match is anchored at start, so "abc$" applied to "xabc" fails at start.
    # search should find it.
    assert not r_end.match("xabc"), "'abc$' should not match 'xabc' using match()"
    assert r_end.search("xabc"), "'abc$' should match 'xabc' using search()"


def test_alternation():
    """测试选择操作"""
    r = Regex("cat|dog")
    assert r.match("cat"), "'cat|dog' should match 'cat'"
    assert r.match("dog"), "'cat|dog' should match 'dog'"
    assert not r.match("bird"), "'cat|dog' should not match 'bird'"


def test_quantifiers():
    """测试量词"""
    # + 一次或多次
    r_plus = Regex("ab+c")
    assert r_plus.match("abc"), "'ab+c' should match 'abc'"
    assert r_plus.match("abbc"), "'ab+c' should match 'abbc'"
    assert not r_plus.match("ac"), "'ab+c' should not match 'ac'"

    # ? 零次或一次
    r_qmark = Regex("ab?c")
    assert r_qmark.match("ac"), "'ab?c' should match 'ac'"
    assert r_qmark.match("abc"), "'ab?c' should match 'abc'"
    assert not r_qmark.match("abbc"), "'ab?c' should not match 'abbc'"


def test_search_vs_match():
    """Test search vs match distinction"""
    r = Regex("abc")
    # match is anchored
    assert r.match("abc")
    assert not r.match("xabc")
    
    # search is unanchored
    assert r.search("abc")
    assert r.search("xabc")
    assert not r.search("ac")


def test_match_object():
    """Test Match object attributes"""
    r = Regex("abc")
    m = r.search("xabcy")
    assert m, "Should match"
    assert m.start() == 1
    assert m.end() == 4
    assert m.span() == (1, 4)
    # Check truthiness
    if m:
        pass
    else:
        assert False, "Match object should be truthy"


if __name__ == "__main__":
    test_basic_match()
    print("test_basic_match passed")

    test_anchors()
    print("test_anchors passed")

    test_alternation()
    print("test_alternation passed")

    test_quantifiers()
    print("test_quantifiers passed")
    
    test_search_vs_match()
    print("test_search_vs_match passed")

    test_match_object()
    print("test_match_object passed")

    print("\nAll Python binding tests passed!")
