#!/bin/sh
# git filter-branch --index-filter 用的脚本。
#
# 它只在一次性的历史重写里用到，不属于构建流程。重写完成后可以删除。
#
# 两步。
#
# 第一步：七个指定文件。★ 路径全部以 :/ 开头锚定到仓库根 —— 不加锚定的
#   'NvpwrControl.exe' 会匹配任意深度的同名文件，包括 release/ 里要保留的那个，
#   以及 app/x64/Release/ 里的构建输出。这一点在旧提交上验证过：未锚定匹配 3 处，
#   锚定只匹配 1 处。
#
# 第二步：构建输出。它们曾被提交过，.gitignore 是后来才加的，而 .gitignore 不会
#   取消跟踪 —— 所以这些中间文件出现在每一个提交里，并且因为 .obj/.pdb 每次编译
#   都不同，每一个版本都作为独立对象存着。nvpwr_ipc.obj 一个文件就有五个版本。
#
#   这里用 git 自己的 pathspec glob，而不是 git ls-files | grep | xargs。后者是
#   第一版写法，在 filter-branch 的 sh 里 grep 和 xargs 都不在 PATH 上，于是整个
#   第二步【静默失效】—— 重写照常完成，日志照常输出，但什么都没删掉，只有事后
#   对比对象列表才能发现。pathspec 由 git 自己解析，没有这个失败模式。

# ---- 第一步：分叉残留 ------------------------------------------------------
git rm -q --cached --ignore-unmatch \
    ':/KDU_EXPLORATION_NOTES.md' \
    ':/NvpwrControl-v1.8.0-RTX-40-50-Series.zip' \
    ':/NvpwrControl.exe' \
    ':/NvpwrCtl.exe' \
    ':/Nvpwr.sys' \
    ':/Nvpwr.cer' \
    ':/mvolt+.exe'

# ---- 第二步：构建输出 ------------------------------------------------------
git rm -q -r --cached --ignore-unmatch \
    ':(glob)**/obj/**' \
    ':(glob)**/bin/**' \
    ':(glob)**/publish/**' \
    ':(glob)**/x64/Release/**' \
    ':(glob)**/x64/Debug/**' \
    ':(glob)**/dist/**' \
    ':(glob)**/*.obj' \
    ':(glob)**/*.pdb' \
    ':(glob)**/*.ilk' \
    ':(glob)**/*.exp' \
    ':(glob)**/*.bak' \
    ':(glob)**/*.bak2' \
    ':(glob)**/*.bak3' \
    ':(glob)**/*.user'
