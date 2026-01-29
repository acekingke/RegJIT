import sys

sys.path.insert(0, "..")
from _regjit import Regex


def test_basic_match():
    r = Regex("ab*c")
    assert r.match("ac")
    assert r.match("abbc")
    assert not r.match("ab")


if __name__ == "__main__":
    test_basic_match()
    print("ok")
