// test_anchor_quant_edge.cpp
// 
// 本项目遵循 Python re 语义。在 Python re 中，对锚点使用量词是非法的：
// - ^*, ^+, ^{2} 等会抛出 "nothing to repeat" 错误
// - $*, $+, ${2} 等同样非法
// - \b*, \B+ 等也是非法的

#include "../src/regjit.h"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "[锚点/量词边缘测试]" << std::endl;
    std::cout << "注意：本项目遵循 Python re 语义" << std::endl;
    std::cout << "在 Python re 中，^*, ^+, $*, \\b* 等模式是非法的" << std::endl;
    std::cout << "这些模式应该在编译时被拒绝" << std::endl;
    std::cout << std::endl;

    Initialize();
    assert(!CompileRegex("^*") && "^* should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("^+") && "^+ should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("^{2}") && "^{2} should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("$*") && "$* should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("$+") && "$+ should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("${2}") && "${2} should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("\\b*") && "\\b* should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("\\B+") && "\\B+ should fail to compile (nothing to repeat)");
    CleanUp();

    Initialize();
    assert(!CompileRegex("\\b{2}") && "\\b{2} should fail to compile (nothing to repeat)");
    CleanUp();

    std::cout << "[锚点/量词边缘测试 - 通过]" << std::endl;
    return 0;
}
