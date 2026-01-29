from _regjit import Regex


def main():
    r = Regex("a+b")
    print("match abc:", r.match("abc"))
    print("match b:", r.match("b"))


if __name__ == "__main__":
    main()
