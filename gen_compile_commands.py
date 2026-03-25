#! python
import subprocess
import sys
import re
import json
import os
import shlex
import argparse
import copy

# --- 核心配置 ---
# 识别的编译器列表（支持正则）
COMPILERS = [
    r"gcc",
    r"g\+\+",
    r"clang",
    r"clang\+\+",
    r"arm-none-eabi-gcc",
    r"arm-none-eabi-g\+\+",
    r"cc",
    r"c\+\+",
    r"riscv64-unknown-elf-gcc",
    r"icc",
    r"icpc",
    r"cl",
    r"cl\.exe",  # MSVC
    r"jar",  # 有时也需要构建 Java/Kotlin 的索引
]
# ----------------


def run_command(cmd, capture=False, env=None):
    """运行命令的通用封装"""

    # 打印提示
    if capture:
        print(f"[CAPTURE] Executing dry-run: {shlex.join(cmd)}")
    else:
        print(f"[BUILD] Executing real build: {shlex.join(cmd)}")

    try:
        if capture:
            # 捕获模式：用于生成数据库 (Dry-Run)
            # 使用 errors='replace' 防止编码错误导致脚本崩溃
            result = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=env,
            )
            return result.stdout.splitlines()
        else:
            # 执行模式：用于真正编译 (Real Build)
            # 直接将输出导向终端，让用户看到实时进度
            result = subprocess.run(cmd, env=env)
            return result.returncode
    except FileNotFoundError:
        print(f"Error: Command '{cmd[0]}' not found in PATH.")
        return None
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
        return None


def get_lines_from_file(filepath):
    """从日志文件读取内容"""
    print(f"[LOG] Parsing log file: {filepath}")
    if not os.path.exists(filepath):
        print(f"Error: File '{filepath}' not found.")
        sys.exit(1)
    try:
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            return f.readlines()
    except Exception as e:
        print(f"Error reading file: {e}")
        sys.exit(1)


def parse_line(line, working_dir, output_format):
    """解析单行输出"""
    # 1. 快速过滤：必须包含源代码后缀，优化性能
    if not re.search(r"\.(c|cpp|cc|cxx|s|asm)\b", line, re.IGNORECASE):
        return None

    try:
        # posix=True 适合解析类 Unix 的 Make/GCC 输出
        # 如果是纯 Windows 环境 (cl.exe + cmd)，可能需要根据实际情况设为 False
        line = line.replace("\\", "/")
        tokens = shlex.split(s=line, posix=True)
    except ValueError:
        return None

    if not tokens:
        return None

    # 2. 判定是否为编译器
    compiler_cmd = tokens[0]
    is_compiler = False
    for comp in COMPILERS:
        if re.search(comp + r"$", compiler_cmd, re.IGNORECASE):
            is_compiler = True
            break

    if not is_compiler:
        return None

    # 3. 提取源文件路径
    file_path = None
    for i, token in enumerate(tokens):
        if token.startswith("-") or token.startswith("/"):
            continue
        # 排除参数值
        if i > 0 and tokens[i - 1] in [
            "-o",
            "-I",
            "-D",
            "-include",
            "-isystem",
            "-MF",
            "-MT",
        ]:
            continue

        if re.search(r"\.(c|cpp|cc|cxx|s|asm)$", token, re.IGNORECASE):
            file_path = os.path.abspath(os.path.join(working_dir, token))
            break

    if not file_path:
        return None

    # 4. 路径标准化 (Windows下统一转为 /)
    file_path = file_path.replace("\\", "/")
    work_dir_norm = working_dir.replace("\\", "/")

    entry = {
        "directory": work_dir_norm,
    }

    if output_format == "arguments":
        entry["arguments"] = tokens
    else:
        entry["command"] = line.strip()
    entry["file"] = file_path
    return entry


def load_existing_db(db_path):
    """加载现有 DB 用于合并"""
    if not os.path.exists(db_path):
        return {}
    try:
        with open(db_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            db_map = {}
            for entry in data:
                if "file" in entry:
                    # 使用标准化路径作为 Key
                    key = entry["file"].replace("\\", "/")
                    db_map[key] = entry
            return db_map
    except:
        return {}


def main():
    parser = argparse.ArgumentParser(
        description="Generate compile_commands.json from Build Logs or Build Commands.",
        epilog="Examples:\n  python gen_compile_commands.py --parse build.log\n  python gen_compile_commands.py make -j8\n  python gen.py --format arguments make",
    )

    # 输入源选项
    parser.add_argument(
        "--parse",
        metavar="LOG_FILE",
        help="Read from a build log file instead of executing a command.",
    )

    # 行为选项
    parser.add_argument(
        "-n",
        "--dry-run",
        action="store_true",
        help="Only generate the DB, do not run the actual build command (ignored if using --parse).",
    )

    parser.add_argument(
        "--format",
        choices=["command", "arguments"],
        default="command",
        help="Output format. 'arguments' is recommended for clangd/vscode.",
    )

    parser.add_argument(
        "--clean",
        action="store_true",
        help="Overwrite existing database instead of incrementaly merging.",
    )

    parser.add_argument(
        "-o", "--output", default="compile_commands.json", help="Output file path."
    )

    # 捕获构建命令
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="The build command to execute (e.g., make all)",
    )

    args = parser.parse_args()
    working_dir = os.getcwd()

    # --- 阶段 1: 获取输入行 (Source Phase) ---
    lines = []
    mode = "unknown"

    # 处理 command 参数，去掉可能存在的 '--'
    build_cmd = args.command
    if build_cmd and build_cmd[0] == "--":
        build_cmd = build_cmd[1:]

    if args.parse:
        # 模式 A: 从日志文件读取
        mode = "log"
        lines = get_lines_from_file(args.parse)
    elif build_cmd:
        # 模式 B: 拦截构建命令
        mode = "command"

        # 构造 Dry-Run 命令
        probe_cmd = copy.deepcopy(build_cmd)
        is_make = "make" in probe_cmd[0].lower()

        # 智能注入 Dry-Run 参数
        if is_make:
            if "-n" not in probe_cmd:
                probe_cmd.append("-n")
            if "-B" not in probe_cmd:
                probe_cmd.append("-B")
        else:
            # 非 make 命令 (如 ninja)，尝试简单注入 -n
            # 如果是 ninja，应该用 '-t', 'commands'，这里只做简单处理
            if "ninja" in probe_cmd[0]:
                probe_cmd = [probe_cmd[0], "-t", "commands"] + probe_cmd[1:]
            elif "-n" not in probe_cmd:
                probe_cmd.append("-n")

        lines = run_command(probe_cmd, capture=True)
        if lines is None:  # 命令执行失败
            sys.exit(1)
    else:
        print("Error: You must provide either a log file (--parse) or a build command.")
        parser.print_help()
        sys.exit(1)

    # --- 阶段 2: 解析与生成 (Parsing Phase) ---
    print(f"--- Processing Input ({len(lines)} lines) ---")

    db_map = {}
    if not args.clean:
        db_map = load_existing_db(args.output)
        if db_map:
            print(f"Loaded {len(db_map)} existing entries from {args.output}")

    count_new = 0
    count_update = 0

    for line in lines:
        line = line.strip()
        if not line:
            continue

        entry = parse_line(line, working_dir, args.format)
        if entry:
            fpath = entry["file"]
            if fpath in db_map:
                db_map[fpath] = entry
                count_update += 1
            else:
                db_map[fpath] = entry
                count_new += 1

    # 排序并保存
    final_entries = sorted(db_map.values(), key=lambda x: x["file"])

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(final_entries, f, indent=4)

    print(f"Success! Generated {args.output}")
    print(
        f"Stats: {len(final_entries)} total, {count_new} added, {count_update} updated."
    )

    # --- 阶段 3: 执行真实构建 (Execution Phase) ---
    # 只有在“命令拦截模式”且未指定“dry-run”时才执行
    if mode == "command" and not args.dry_run:
        print("\n--- Running Actual Build ---")
        ret_code = run_command(build_cmd, capture=False)
        sys.exit(ret_code)


if __name__ == "__main__":
    main()
